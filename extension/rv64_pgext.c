/*
 * rv64_pgext.c — PostgreSQL extension: RISC-V VM as SQL-queryable state
 *
 * Embeds the rv64 emulator inside PostgreSQL. Each VM instance is a
 * static struct that can be queried and stepped through SQL.
 *
 * Functions:
 *   vm_boot(fw, kernel, dtb, initrd, disk) → TEXT   -- boot log
 *   vm_step(cycles BIGINT) → BIGINT                 -- instructions executed
 *   vm_state() → RECORD                             -- CPU/PLIC/virtio state
 *   vm_send(input TEXT) → void                      -- send to console
 *   vm_console() → TEXT                             -- read console output
 *
 * Build:
 *   make -f Makefile.vm install
 *   CREATE EXTENSION linuxsql_vm;
 */

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "catalog/pg_type.h"
#include "access/htup_details.h"
#include "utils/array.h"
#include "utils/memutils.h"
#include "executor/spi.h"
#include "storage/ipc.h"
#include "storage/large_object.h"
#include "libpq/libpq-fs.h"
#include "access/xact.h"
#include "utils/snapmgr.h"

/* The emulator is plain C — no header conflicts with PG */
#include "rv64_cpu.h"
#include "rv64_jit.h"

#include "access/parallel.h"
#include "storage/dsm.h"
#include "storage/shm_toc.h"
#include "storage/shm_mq.h"

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

PG_MODULE_MAGIC;

/* ---- Global VM instance ---- */

static struct rv64_cpu *vm_cpu = NULL;
static uint8_t *vm_ram = NULL;
static dsm_segment *vm_ram_dsm = NULL;
static uint8_t *vm_disk = NULL;

/* Frontend-side RAM mapping (lazily attached via DSM handle from SharedVMState) */
static dsm_segment *frontend_ram_seg = NULL;
static uint8_t *frontend_ram = NULL;

uint8_t *get_guest_ram_ptr(void);
uint8_t *get_guest_ram_ptr(void) {
    if (frontend_ram) return frontend_ram;
    return vm_ram;
}
static uint64_t vm_disk_size = 0;

/*
 * Dirty-block bitmap for write-back disk persistence.
 * One bit per 4KB block; set on every virtio write.
 * Cleared by vm_disk_flush() after UPSERTing to vm_blocks.
 */
static uint8_t *vm_disk_dirty = NULL;
#define DISK_BLOCK_SIZE 4096ULL
#define DISK_DIRTY_SET(idx) \
	do { if (vm_disk_dirty) vm_disk_dirty[(idx) >> 3] |= (uint8_t)(1 << ((idx) & 7)); } while (0)
#define DISK_DIRTY_GET(idx) \
	(vm_disk_dirty ? (vm_disk_dirty[(idx) >> 3] & (1 << ((idx) & 7))) : 0)
#define DISK_DIRTY_CLR(idx) \
	do { if (vm_disk_dirty) vm_disk_dirty[(idx) >> 3] &= (uint8_t)~(1 << ((idx) & 7)); } while (0)

/* ---- Background Worker IPC State ---- */

typedef enum {
    VM_CMD_IDLE = 0,
    VM_CMD_BOOT = 1,
    VM_CMD_STEP = 2,
    VM_CMD_HALT = 3,
    VM_CMD_SAVE = 4,
    VM_CMD_SEND = 5,
    VM_CMD_WATCHPOINT = 6,
    VM_CMD_BREAKPOINT = 7
} VmIPCCommand;

typedef struct {
    uint32_t magic;
    slock_t mutex;
    
    pid_t worker_pid;
    bool worker_ready;
    bool shutdown_requested;
    
    /* Request from client -> worker */
    VmIPCCommand cmd;
    int64_t cmd_arg;     /* E.g. cycles for step */
    int64_t cmd_arg_ext; /* Additional argument (e.g. scale/size) */
    
    /* Result from worker -> client */
    int cmd_result;      /* 0 = OK, -1 = Error */
    int64_t cmd_return;  /* E.g. executed cycles */
    char error_msg[256]; /* Fatal error strings */
    
    char input_buf[1024];
    
#define CONSOLE_BUF_SIZE (1024 * 1024)
    int console_pos;
    char console_buf[CONSOLE_BUF_SIZE];
    
    
    /* Latches for precise native polling */
    Latch *worker_latch;
    Latch *client_latch;
    
    /* Virtual Machine Copy state (updated regularly to decouple readers) */
    struct rv64_cpu cpu;

    /* RAM DSM handle — allows frontend processes to attach and read guest memory */
    dsm_handle ram_dsm_handle;
    uint64_t   ram_size;
    bool       ram_handle_valid;

    /* JIT config requests from frontend (applied by worker before each run) */
    int32_t jit_block_size_request;  /* -1 = no pending change */
    uint64_t jit_disabled_ops[2];    /* bitmask: opcodes 0..127 */
    bool jit_disabled_ops_dirty;     /* true = worker needs to sync from shared state */
    
} SharedVMState;

static dsm_segment *vm_dsm = NULL;
static SharedVMState *vm_shared_state = NULL;
#define VM_IPC_MAGIC 0xBEEF8899

/*
 * ensure_worker_attached() — Try to discover and attach to a running background worker
 */
static void ensure_worker_attached(void) {
    if (vm_shared_state && vm_shared_state->worker_ready) return;

    if (vm_dsm != NULL) {
        /* We have a DSM but no shared_state? Should not happen. */
        return;
    }

    int ret;
    SPI_connect();
    /* Handle might be null/missing if table was just created or never updated */
    ret = SPI_execute("SELECT vm_dsm_handle, vm_worker_pid FROM vm_state WHERE id = 1", true, 1);
    
    if (ret == SPI_OK_SELECT && SPI_processed == 1) {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull1 = true, isnull2 = true;
        Datum d1 = 0, d2 = 0;
        
        int fnum1 = SPI_fnumber(tupdesc, "vm_dsm_handle");
        int fnum2 = SPI_fnumber(tupdesc, "vm_worker_pid");
        
        if (fnum1 > 0 && fnum2 > 0) {
            d1 = SPI_getbinval(tuple, tupdesc, fnum1, &isnull1);
            d2 = SPI_getbinval(tuple, tupdesc, fnum2, &isnull2);
        }
        
        if (!isnull1 && !isnull2) {
            uint64_t handle = DatumGetInt64(d1);
            int pid = DatumGetInt32(d2);
            
            if (pid > 0 && kill(pid, 0) == 0) {
                vm_dsm = dsm_attach((dsm_handle)handle);
                if (vm_dsm) {
                    dsm_pin_mapping(vm_dsm);
                    vm_shared_state = (SharedVMState *)dsm_segment_address(vm_dsm);
                    if (vm_shared_state && vm_shared_state->magic == VM_IPC_MAGIC) {
                        SPI_finish();
                        return;
                    } else {
                        dsm_unpin_mapping(vm_dsm);
                        dsm_detach(vm_dsm);
                        vm_dsm = NULL;
                        vm_shared_state = NULL;
                    }
                }
            }
        }
    }
    SPI_finish();
}

/*
 * ensure_frontend_ram() — Lazily attach to the RAM DSM segment from the frontend.
 *
 * The worker creates guest RAM as a DSM segment and exports its handle
 * in SharedVMState. The frontend calls this to get its own valid mapping.
 * After this call, vm_shared_state->cpu.ram points to valid memory in
 * the frontend's address space, so bus_load / rv64_cpu_load work
 * transparently from any SQL function (vm_disasm, vm_find, vm_physpeek, etc).
 */
static void ensure_frontend_ram(void)
{
	ensure_worker_attached();

	if (!vm_shared_state || !vm_shared_state->ram_handle_valid)
		elog(ERROR, "VM RAM not available (not booted?)");

	if (!frontend_ram) {
		frontend_ram_seg = dsm_attach(vm_shared_state->ram_dsm_handle);
		if (!frontend_ram_seg)
			elog(ERROR, "Failed to attach to VM RAM DSM segment");
		dsm_pin_mapping(frontend_ram_seg);
		frontend_ram = dsm_segment_address(frontend_ram_seg);
	}

	/* Patch the shared cpu copy so bus_load/rv64_cpu_load work transparently */
	vm_shared_state->cpu.ram_size = vm_shared_state->ram_size;
}

/*
 * Real-time boot anchor for clint.mtime synchronization.
 * Captured via CLOCK_MONOTONIC at boot/resume so that clint.mtime
 * tracks wall-clock nanoseconds (in units of 100 ns = 10 MHz ticks)
 * rather than emulated instruction cycles.
 */
uint64_t vm_boot_time_ns = 0;

static inline void set_boot_time_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	vm_boot_time_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* UART TX callback: append to console buffer */
static void pg_uart_tx(uint8_t ch, void *opaque)
{
    fprintf(stderr, "%c", ch);
    fflush(stderr);
	if (vm_shared_state) {
		int pos = vm_shared_state->console_pos;
		if (pos < CONSOLE_BUF_SIZE - 1) {
			vm_shared_state->console_buf[pos] = ch;
			vm_shared_state->console_buf[pos + 1] = '\0';
			vm_shared_state->console_pos = pos + 1;
		}

		/* Log instret at each newline for console-instret correlation */
		if (ch == '\n' && vm_cpu) {
			int slot = vm_cpu->dbg.console_log_count % DBG_CONSOLE_LOG_SIZE;
			vm_cpu->dbg.console_log[slot].instret = vm_cpu->instret;
			vm_cpu->dbg.console_log[slot].console_pos = vm_shared_state->console_pos;
			vm_cpu->dbg.console_log_count++;
		}
	}
}

/* Virtio dirty-block callback: sets bits in vm_disk_dirty for every write.
 * Called from virtio_blk_process_queue — no SPI, just bit manipulation. */
static void vm_dirty_callback(uint64_t byte_offset, uint64_t length, void *opaque)
{
	(void)opaque;
	if (!vm_disk_dirty || length == 0) return;
	uint64_t blk_start = byte_offset / DISK_BLOCK_SIZE;
	uint64_t blk_end   = (byte_offset + length + DISK_BLOCK_SIZE - 1) / DISK_BLOCK_SIZE;
	for (uint64_t b = blk_start; b < blk_end; b++)
		DISK_DIRTY_SET(b);
}

/* ---- Large Object helpers for streaming read/write ---- */

/* Write a buffer to a large object, creating it if oid==InvalidOid.
 * Returns the OID of the (possibly newly created) large object. */
static Oid lo_write_buf(const uint8_t *buf, uint64_t len, Oid existing_oid)
{
	Oid oid;
	if (OidIsValid(existing_oid)) {
		/* Truncate and reuse */
		oid = existing_oid;
	} else {
		oid = inv_create(InvalidOid);
	}

	LargeObjectDesc *lod = inv_open(oid, INV_WRITE, CurrentMemoryContext);

	/* Write in 1MB chunks to avoid huge palloc */
	const uint64_t chunk = 1024 * 1024;
	uint64_t off = 0;
	while (off < len) {
		uint64_t n = (len - off > chunk) ? chunk : (len - off);
		inv_write(lod, (const char *)(buf + off), (int)n);
		off += n;
	}

	inv_close(lod);
	return oid;
}

/* ================================================================
 * BACKGROUND WORKER EXECUTION DAEMON
 * ================================================================ */

#include "postmaster/bgworker.h"
#include "storage/proc.h"
#include "utils/wait_event.h"
#include "miscadmin.h"
#include "tcop/tcopprot.h"

static volatile sig_atomic_t got_sigterm = false;
static void vm_worker_sigterm(SIGNAL_ARGS) {
    int save_errno = errno;
    got_sigterm = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

static void worker_flush_dirty_blocks(void) {
    if (!vm_disk_dirty || vm_disk_size == 0) return;

    StartTransactionCommand();
    PushActiveSnapshot(GetTransactionSnapshot());
    
    if (SPI_connect() != SPI_OK_CONNECT) {
        PopActiveSnapshot();
        CommitTransactionCommand();
        return;
    }

    Oid argtypes[2] = {INT8OID, BYTEAOID};
    Datum values[2];
    char nulls[2] = {' ', ' '};
    
    int flushed = 0;
    uint64_t nblocks = (vm_disk_size + DISK_BLOCK_SIZE - 1) / DISK_BLOCK_SIZE;

    for (uint64_t b = 0; b < nblocks; b++) {
        if (DISK_DIRTY_GET(b)) {
            DISK_DIRTY_CLR(b);
            
            uint64_t off = b * DISK_BLOCK_SIZE;
            uint64_t blen = DISK_BLOCK_SIZE;
            if (off + blen > vm_disk_size) blen = vm_disk_size - off;
            
            bytea *bdata = (bytea *)palloc(VARHDRSZ + blen);
            SET_VARSIZE(bdata, VARHDRSZ + blen);
            memcpy(VARDATA(bdata), vm_disk + off, blen);
            
            values[0] = Int64GetDatum((int64_t)b);
            values[1] = PointerGetDatum(bdata);
            
            SPI_execute_with_args(
                "INSERT INTO vm_blocks (vm_id, blk_idx, data) VALUES (1, $1, $2) "
                "ON CONFLICT (vm_id, blk_idx) DO UPDATE SET data = EXCLUDED.data",
                2, argtypes, values, nulls, false, 0
            );
            
            pfree(bdata);
            flushed++;
            
            /* Chunk commits to prevent massive subtransaction bloat natively */
            if (flushed >= 256) {
               break; 
            }
        }
    }
    
    SPI_finish();
    PopActiveSnapshot();
    CommitTransactionCommand();
}

static void worker_handle_cmd_boot(SharedVMState *st);
PGDLLEXPORT void linuxsql_vm_worker_main(Datum main_arg);

void linuxsql_vm_worker_main(Datum main_arg) {
    pqsignal(SIGTERM, vm_worker_sigterm);
    BackgroundWorkerUnblockSignals();

    /* Connect strictly to local database instance */
    BackgroundWorkerInitializeConnection("linuxsql", NULL, 0);

    /* Map shared IPC structure passed dynamically */
    uint32_t handle_val = DatumGetUInt32(main_arg);
    vm_dsm = dsm_attach(handle_val);
    if (!vm_dsm) {
        elog(ERROR, "LinuxSQL worker failed to attach DSM %u", handle_val);
    }
    vm_shared_state = (SharedVMState *)dsm_segment_address(vm_dsm);
    if (vm_shared_state->magic != VM_IPC_MAGIC) {
        elog(ERROR, "Corrupt SharedVMState magically mapped.");
    }

    SpinLockAcquire(&vm_shared_state->mutex);
    vm_shared_state->worker_pid = MyProcPid;
    vm_shared_state->worker_latch = MyLatch;
    vm_shared_state->worker_ready = true;
    SpinLockRelease(&vm_shared_state->mutex);

    elog(LOG, "LinuxSQL Background Worker %d dynamically initialized", MyProcPid);

    bool running = true;
    while (running && !got_sigterm) {
        ResetLatch(MyLatch);
        CHECK_FOR_INTERRUPTS();

        int flush = 0;
        VmIPCCommand cmd = VM_CMD_IDLE;
        
        SpinLockAcquire(&vm_shared_state->mutex);
        cmd = vm_shared_state->cmd;
        if (vm_shared_state->shutdown_requested) {
            running = false;
        }
        SpinLockRelease(&vm_shared_state->mutex);

        if (!running) break;

        if (cmd == VM_CMD_BOOT) {
            worker_handle_cmd_boot(vm_shared_state);
            flush = 1;
        } else if (cmd == VM_CMD_STEP) {
            /* Synchronous stepping logic natively */
            if (vm_cpu) {
            	/* Sync debug state from frontend before running */
            	vm_cpu->dbg.debug_halt = vm_shared_state->cpu.dbg.debug_halt;

            	/* Apply pending JIT config from frontend */
            	if (vm_shared_state->jit_block_size_request >= 0 && vm_cpu->jit) {
            		vm_cpu->jit->rt_max_block_insns = vm_shared_state->jit_block_size_request;
            		jit_flush(vm_cpu->jit);
            		vm_shared_state->jit_block_size_request = -1;
            	}
            	if (vm_shared_state->jit_disabled_ops_dirty && vm_cpu->jit) {
            		vm_cpu->jit->disabled_ops[0] = vm_shared_state->jit_disabled_ops[0];
            		vm_cpu->jit->disabled_ops[1] = vm_shared_state->jit_disabled_ops[1];
            		jit_flush(vm_cpu->jit);
            		vm_shared_state->jit_disabled_ops_dirty = false;
            	}

            	uint64_t before = vm_cpu->instret;
            	rv64_run(vm_cpu, (uint64_t)vm_shared_state->cmd_arg);
            	uint64_t executed = vm_cpu->instret - before;
            	
            	SpinLockAcquire(&vm_shared_state->mutex);
            	vm_shared_state->cmd_return = executed;
            	vm_shared_state->cpu.instret = vm_cpu->instret;
            	vm_shared_state->cpu.pc = vm_cpu->pc;
            	vm_shared_state->cpu.error = vm_cpu->error;
            	vm_shared_state->cmd_result = 0;
            	SpinLockRelease(&vm_shared_state->mutex);
            } else {
            	SpinLockAcquire(&vm_shared_state->mutex);
            	vm_shared_state->cmd_result = -1;
            	snprintf(vm_shared_state->error_msg, sizeof(vm_shared_state->error_msg), "VM not initialized in worker");
            	SpinLockRelease(&vm_shared_state->mutex);
            }

            /* Unblock the client immediately before expensive I/O operations */
            SpinLockAcquire(&vm_shared_state->mutex);
            vm_shared_state->cmd = VM_CMD_IDLE;
            Latch *client = vm_shared_state->client_latch;
            if (vm_cpu && client) {
                memcpy(&vm_shared_state->cpu, vm_cpu, sizeof(struct rv64_cpu));
            }
            SpinLockRelease(&vm_shared_state->mutex);
            
            if (client) SetLatch(client);
            
            /* No need for outer flush logic to handle this command anymore */
            flush = 0;

            /* Check for dirty blocks and flush natively decoupled from frontend frontend */
            if (vm_disk_dirty) {
                worker_flush_dirty_blocks();
            }
        } else if (cmd == VM_CMD_HALT) {
            running = false;
            flush = 1;
        } else if (cmd == VM_CMD_SEND) {
            for (int i = 0; vm_shared_state->input_buf[i]; i++) {
                if (vm_cpu) rv64_uart_rx(vm_cpu, (uint8_t)vm_shared_state->input_buf[i]);
            }
            flush = 1;
        } else if (cmd == VM_CMD_WATCHPOINT) {
            if (vm_cpu) {
                vm_cpu->dbg.watch_addr = (uint64_t)vm_shared_state->cmd_arg;
                vm_cpu->dbg.watch_size = (uint32_t)vm_shared_state->cmd_arg_ext;
                vm_cpu->dbg.watch_active = (vm_cpu->dbg.watch_addr != 0);
                vm_cpu->dbg.store_log_count = 0;
                memset(vm_cpu->dbg.store_log, 0, sizeof(vm_cpu->dbg.store_log));
                vm_shared_state->cmd_result = 0;
            } else {
                vm_shared_state->cmd_result = -1;
            }
            flush = 1;
        } else if (cmd == VM_CMD_BREAKPOINT) {
            if (vm_cpu) {
                vm_cpu->dbg.break_pc = (uint64_t)vm_shared_state->cmd_arg;
                vm_cpu->dbg.break_halt = (int)vm_shared_state->cmd_arg_ext;
                vm_cpu->dbg.break_active = (vm_cpu->dbg.break_pc != 0);
                vm_cpu->dbg.break_log_count = 0;
                vm_cpu->dbg.debug_halt = 0;
                memset(vm_cpu->dbg.break_log, 0, sizeof(vm_cpu->dbg.break_log));
                vm_shared_state->cmd_result = 0;
            } else {
                vm_shared_state->cmd_result = -1;
            }
            flush = 1;
        }

        if (flush) {
            SpinLockAcquire(&vm_shared_state->mutex);
            vm_shared_state->cmd = VM_CMD_IDLE;
            Latch *client = vm_shared_state->client_latch;
            
            /* Copy current CPU state for decoupled reader consumption */
            if (vm_cpu && client) {
                memcpy(&vm_shared_state->cpu, vm_cpu, sizeof(struct rv64_cpu));
            }
            
            SpinLockRelease(&vm_shared_state->mutex);
            
            if (client) SetLatch(client);
        } else {
            /* Idle blocking sleep until requested */
            WaitLatch(MyLatch, WL_LATCH_SET | WL_POSTMASTER_DEATH | WL_TIMEOUT, 1000, PG_WAIT_EXTENSION);
        }
    }

    /* Shutdown sequentially */
    SpinLockAcquire(&vm_shared_state->mutex);
    vm_shared_state->worker_ready = false;
    vm_shared_state->shutdown_requested = true;
    Latch *cl = vm_shared_state->client_latch;
    SpinLockRelease(&vm_shared_state->mutex);
    
    if (cl) SetLatch(cl);

	if (vm_cpu) { pfree(vm_cpu); vm_cpu = NULL; }
    
    dsm_detach(vm_dsm);
    proc_exit(0);
}

/* Read a large object into a pre-allocated buffer.
 * Returns bytes read, or -1 on error. */
static int64_t lo_read_buf(Oid oid, uint8_t *buf, uint64_t len)
{
	if (!OidIsValid(oid))
		return -1;

	LargeObjectDesc *lod = inv_open(oid, INV_READ, CurrentMemoryContext);

	const uint64_t chunk = 1024 * 1024;
	uint64_t off = 0;
	while (off < len) {
		uint64_t want = (len - off > chunk) ? chunk : (len - off);
		int got = inv_read(lod, (char *)(buf + off), (int)want);
		if (got <= 0) break;
		off += got;
	}

	inv_close(lod);
	return (int64_t)off;
}

/*
 * vm_state_save() -> TEXT
 * Explicitly saves the VM CPU state + RAM into vm_state table.
 */
PG_FUNCTION_INFO_V1(rv64_vm_state_save);
Datum
rv64_vm_state_save(PG_FUNCTION_ARGS)
{
	int64 vm_id = 1;
	if (PG_NARGS() > 0 && !PG_ARGISNULL(0))
		vm_id = PG_GETARG_INT64(0);

	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready) {
		elog(ERROR, "vm_state_save: no VM running");
	}
	ensure_frontend_ram();

	MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

	if (SPI_connect() != SPI_OK_CONNECT) {
		MemoryContextSwitchTo(old);
		elog(ERROR, "vm_state_save: SPI_connect failed");
	}

	/* Read existing ram_oid so we can reuse it if it exists */
	Oid ram_oid = InvalidOid;
	Oid disk_oid = InvalidOid; /* Kept for schema compatibility, but zeroed/ignored */

	char qbuf[256];
	snprintf(qbuf, sizeof(qbuf), "SELECT ram_oid FROM vm_state WHERE id = %lld", (long long)vm_id);
	int ret = SPI_execute(qbuf, true, 1);
	if (ret == SPI_OK_SELECT && SPI_processed == 1) {
		bool isnull;
		Datum d;
		d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
		if (!isnull) ram_oid = DatumGetObjectId(d);
	}

	/* Write RAM to large object (do NOT write 2GB disk to LO, vm_blocks handles it) */
	ram_oid = lo_write_buf(frontend_ram, vm_shared_state->cpu.ram_size, ram_oid);

	uint64_t cpu_sz = sizeof(struct rv64_cpu);

	/* Upsert vm_state row via SPI */
	{
		StringInfoData query;
		initStringInfo(&query);

		appendStringInfo(&query,
			"INSERT INTO vm_state (id, cpu_state, console, ram_oid, disk_oid, "
			"ram_size, disk_size, instret, pc, cycle, saved_at) "
			"VALUES (%lld, $1, $2, %u, %u, %lu, %lu, %lu, %lu, %lu, now()) "
			"ON CONFLICT (id) DO UPDATE SET "
			"cpu_state = EXCLUDED.cpu_state, console = EXCLUDED.console, "
			"ram_oid = EXCLUDED.ram_oid, disk_oid = EXCLUDED.disk_oid, "
			"ram_size = EXCLUDED.ram_size, disk_size = EXCLUDED.disk_size, "
			"instret = EXCLUDED.instret, pc = EXCLUDED.pc, "
			"cycle = EXCLUDED.cycle, saved_at = EXCLUDED.saved_at",
			(long long)vm_id,
			(unsigned int)ram_oid,
			(unsigned int)InvalidOid, /* Disable full disk LOs */
			(unsigned long)vm_shared_state->cpu.ram_size,
			(unsigned long)vm_disk_size,
			(unsigned long)vm_shared_state->cpu.instret,
			(unsigned long)vm_shared_state->cpu.pc,
			(unsigned long)vm_shared_state->cpu.cycle);

		/* Prepare bytea datums */
		Oid argtypes[2] = { BYTEAOID, BYTEAOID };
		Datum values[2];
		char nulls[2] = { ' ', ' ' };

		/* cpu_state bytea */
		bytea *cpu_bytea = (bytea *)palloc(VARHDRSZ + cpu_sz);
		SET_VARSIZE(cpu_bytea, VARHDRSZ + cpu_sz);
		memcpy(VARDATA(cpu_bytea), &vm_shared_state->cpu, cpu_sz);
		values[0] = PointerGetDatum(cpu_bytea);

		/* console bytea */
		bytea *con_bytea = (bytea *)palloc(VARHDRSZ + vm_shared_state->console_pos);
		SET_VARSIZE(con_bytea, VARHDRSZ + vm_shared_state->console_pos);
		memcpy(VARDATA(con_bytea), vm_shared_state->console_buf, vm_shared_state->console_pos);
		values[1] = PointerGetDatum(con_bytea);

		int spi_rc = SPI_execute_with_args(query.data, 2, argtypes, values, nulls, false, 0);
		if (spi_rc != SPI_OK_INSERT && spi_rc != SPI_OK_UPDATE) {
			SPI_finish();
			MemoryContextSwitchTo(old);
			elog(ERROR, "vm_state_save: SPI_execute_with_args failed");
		}
	}

	SPI_finish();
	MemoryContextSwitchTo(old);

	PG_RETURN_TEXT_P(cstring_to_text("State saved."));
}

/*
 * Try to restore VM state from the vm_state table.
 * Returns true if state was successfully restored, false if no saved state.
 */
static bool vm_auto_resume(void)
{
	int ret;
	bool resumed = false;

	PushActiveSnapshot(GetTransactionSnapshot());
	SPI_connect();

	ret = SPI_execute(
		"SELECT cpu_state, console, ram_oid, disk_oid, ram_size, disk_size "
		"FROM vm_state WHERE id = 1", true, 1);

	if (ret != SPI_OK_SELECT || SPI_processed != 1)
		goto done;

	{
		HeapTuple tuple = SPI_tuptable->vals[0];
		TupleDesc tupdesc = SPI_tuptable->tupdesc;
		bool isnull;
		Datum d;

		/* Get sizes */
		d = SPI_getbinval(tuple, tupdesc, 5, &isnull); /* ram_size */
		if (isnull) goto done;
		uint64_t saved_ram_size = DatumGetInt64(d);

		d = SPI_getbinval(tuple, tupdesc, 6, &isnull); /* disk_size */
		uint64_t saved_disk_size = isnull ? 0 : DatumGetInt64(d);

		/* Get OIDs */
		d = SPI_getbinval(tuple, tupdesc, 3, &isnull); /* ram_oid */
		if (isnull) goto done;
		Oid ram_oid = DatumGetObjectId(d);

		d = SPI_getbinval(tuple, tupdesc, 4, &isnull); /* disk_oid */
		Oid disk_oid = isnull ? InvalidOid : DatumGetObjectId(d);

		/* Get cpu_state bytea */
		d = SPI_getbinval(tuple, tupdesc, 1, &isnull); /* cpu_state */
		if (isnull) goto done;
		bytea *cpu_bytea = DatumGetByteaP(d);
		if (VARSIZE_ANY_EXHDR(cpu_bytea) != sizeof(struct rv64_cpu)) {
			elog(WARNING, "vm_auto_resume: cpu_state size mismatch (%ld vs %ld)",
			     (long)VARSIZE_ANY_EXHDR(cpu_bytea),
			     (long)sizeof(struct rv64_cpu));
			goto done;
		}

		/* Allocate in TopMemoryContext */
		MemoryContext oldctx = MemoryContextSwitchTo(TopMemoryContext);

		/* Clean up any existing state */
		if (vm_cpu)  { pfree(vm_cpu);  vm_cpu = NULL; }
		if (vm_ram)  { pfree(vm_ram);  vm_ram = NULL; }
		if (vm_disk) { free(vm_disk); vm_disk = NULL; }

		/* Restore CPU struct */
		vm_cpu = palloc(sizeof(struct rv64_cpu));
		memcpy(vm_cpu, VARDATA_ANY(cpu_bytea), sizeof(struct rv64_cpu));

		/* Restore RAM from large object */
		vm_ram = palloc0(saved_ram_size);
		lo_read_buf(ram_oid, vm_ram, saved_ram_size);

		/* Restore console */
		d = SPI_getbinval(tuple, tupdesc, 2, &isnull); /* console */
		if (!isnull) {
			bytea *con_bytea = DatumGetByteaP(d);
			if (con_bytea && VARSIZE_ANY_EXHDR(con_bytea) > 0) {
				vm_shared_state->console_pos = (int)VARSIZE_ANY_EXHDR(con_bytea);
				if (vm_shared_state->console_pos > CONSOLE_BUF_SIZE - 1) {
					vm_shared_state->console_pos = CONSOLE_BUF_SIZE - 1;
				}
				memcpy(vm_shared_state->console_buf, VARDATA_ANY(con_bytea), vm_shared_state->console_pos);
				vm_shared_state->console_buf[vm_shared_state->console_pos] = '\0';
			} else {
				vm_shared_state->console_pos = 0;
				vm_shared_state->console_buf[0] = '\0';
			}
		} else {
			vm_shared_state->console_pos = 0;
			vm_shared_state->console_buf[0] = '\0';
		}

		/* Restore disk from large object */
		if (OidIsValid(disk_oid) && saved_disk_size > 0) {
			vm_disk = malloc(saved_disk_size);
			vm_disk_size = saved_disk_size;
			lo_read_buf(disk_oid, vm_disk, saved_disk_size);
		}

		MemoryContextSwitchTo(oldctx);

		/* Fix up internal pointers — these are host addresses that
		 * changed between the save and this new process */
		/* Removed explicit vm_cpu->ram assignment to respect process locality */
#ifndef DISABLE_JIT
		vm_cpu->jit = calloc(1, sizeof(struct jit_state));
		if (vm_cpu->jit && jit_init(vm_cpu->jit) < 0) {
			free(vm_cpu->jit);
			vm_cpu->jit = NULL;
		}
#else
		vm_cpu->jit = NULL;
#endif
		vm_cpu->uart.tx_callback = pg_uart_tx;
		vm_cpu->uart.tx_opaque = NULL;

		/* Flush TLB — host_base pointers are stale */
		memset(vm_cpu->tlb, 0, sizeof(vm_cpu->tlb));

		/* Re-attach disk to virtio device */
		if (vm_disk) {
			vm_cpu->virtio_blk.disk_data = vm_disk;
			vm_cpu->virtio_blk.disk_size = vm_disk_size;
		}

		/* Clear debug halt so the VM is ready to run */
		vm_cpu->dbg.debug_halt = 0;

		/* Re-anchor the real-time clock */
		set_boot_time_ns();

		resumed = true;

		elog(LOG, "linuxsql: VM resumed (instret=%lu, pc=0x%lx)",
		     (unsigned long)vm_cpu->instret,
		     (unsigned long)vm_cpu->pc);
	}

done:
	SPI_finish();
	close_lo_relation(true);
	PopActiveSnapshot();
	return resumed;
}

/*
 * _PG_init — called when the extension shared library is loaded.
 * Registers a before_shmem_exit callback to auto-save VM state.
 */
void _PG_init(void)
{
	/* No-op. Previously registered before_shmem_exit. */
}

/* Load a file into RAM at the given physical address */
static long load_to_ram(uint8_t *ram, uint64_t ram_size,
                        uint64_t load_addr, const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	struct stat st;
	if (fstat(fd, &st) < 0) {
		close(fd);
		return -1;
	}

	long size = st.st_size;
	uint64_t offset = load_addr - RAM_BASE;
	if (offset + size > ram_size) {
		close(fd);
		return -1;
	}

	ssize_t n = read(fd, ram + offset, size);
	close(fd);
	return (n == size) ? size : -1;
}

/* ---- SQL Functions ---- */

/*
 * vm_boot(fw TEXT, kernel TEXT, dtb TEXT, initrd TEXT, disk TEXT DEFAULT NULL, ram_size_mb INT DEFAULT 2048)
 *
 * Initialize the VM with firmware, kernel, DTB, and initramfs.
 * Optional disk image for virtio-blk.
 * Returns boot status text.
 */
PG_FUNCTION_INFO_V1(rv64_vm_boot);
Datum
rv64_vm_boot(PG_FUNCTION_ARGS)
{
	char *fw_path     = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char *kern_path   = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char *dtb_path    = text_to_cstring(PG_GETARG_TEXT_PP(2));
	char *initrd_path = text_to_cstring(PG_GETARG_TEXT_PP(3));
	char *disk_path   = PG_ARGISNULL(4) ? NULL :
	                    text_to_cstring(PG_GETARG_TEXT_PP(4));
	int ram_size_mb   = PG_ARGISNULL(5) ? 2048 : PG_GETARG_INT32(5);

	/* If VM is already loaded in this session, just return status */
	if (vm_cpu) {
		char result[256];
		snprintf(result, sizeof(result),
		         "VM already running (instret=%lu, pc=0x%lx)",
		         (unsigned long)vm_cpu->instret,
		         (unsigned long)vm_cpu->pc);
		pfree(fw_path); pfree(kern_path); pfree(dtb_path); pfree(initrd_path);
		if (disk_path) pfree(disk_path);
		PG_RETURN_TEXT_P(cstring_to_text(result));
	}

	/* Try to resume from saved state in vm_state table */
	if (vm_auto_resume()) {
		char result[256];
		snprintf(result, sizeof(result),
		         "VM resumed (instret=%lu, pc=0x%lx, ram=%luMB, disk=%luMB)",
		         (unsigned long)vm_cpu->instret,
		         (unsigned long)vm_cpu->pc,
		         (unsigned long)(vm_cpu->ram_size / (1024*1024)),
		         (unsigned long)(vm_disk_size / (1024*1024)));
		pfree(fw_path); pfree(kern_path); pfree(dtb_path); pfree(initrd_path);
		if (disk_path) pfree(disk_path);
		PG_RETURN_TEXT_P(cstring_to_text(result));
	}

	/* No saved state — cold boot */

	/*
	 * Allocate in TopMemoryContext so state persists across
	 * SQL function calls within the same backend session.
	 */
	MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

	/* Clean up previous DSM if left dangling */
	if (vm_ram_dsm) {
		dsm_unpin_mapping(vm_ram_dsm);
		dsm_detach(vm_ram_dsm);
		vm_ram_dsm = NULL;
	}

	uint64_t ram_size = (uint64_t)ram_size_mb * 1024 * 1024;
	
	vm_ram_dsm = dsm_create(ram_size, 0);
	if (!vm_ram_dsm) {
		pfree(fw_path); pfree(kern_path); pfree(dtb_path); pfree(initrd_path);
		if (disk_path) pfree(disk_path);
		ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("Failed to allocate 2GB DSM for vm_ram")));
	}
	
	dsm_pin_mapping(vm_ram_dsm); /* Pin locally so we don't unmap at transaction EOXact */
	vm_ram = dsm_segment_address(vm_ram_dsm);
	memset(vm_ram, 0, ram_size);

	vm_shared_state->console_pos = 0;

	/* Load files */
	long fw_sz   = load_to_ram(vm_ram, ram_size, 0x80000000ULL, fw_path);
	long kern_sz = load_to_ram(vm_ram, ram_size, 0x80200000ULL, kern_path);
	long dtb_sz  = load_to_ram(vm_ram, ram_size, 0x82000000ULL, dtb_path);
	long init_sz = load_to_ram(vm_ram, ram_size, 0x82200000ULL, initrd_path);

	if (fw_sz < 0 || kern_sz < 0 || dtb_sz < 0 || init_sz < 0)
		elog(ERROR, "vm_boot: failed to load VM images");

	/* Patch DTB: update linux,initrd-end to match actual initrd size.
	 * This avoids needing to manually sync the DTS every time the
	 * initramfs is rebuilt. The FDT stores property names as strings
	 * in the strings block; we search for "linux,initrd-end" and then
	 * find the property value referencing that string offset to patch it.
	 *
	 * Simpler approach: scan the DTB in RAM for the property name string,
	 * get its offset in the strings block, then scan the struct block for
	 * FDT_PROP tokens referencing that string offset. */
	{
		uint8_t *dtb = vm_ram + (0x82000000ULL - RAM_BASE);
		uint64_t initrd_end_addr = 0x82200000ULL + init_sz;

		/* FDT header: big-endian */
		uint32_t magic = (dtb[0]<<24)|(dtb[1]<<16)|(dtb[2]<<8)|dtb[3];
		if (magic == 0xd00dfeed) {
			uint32_t totalsize = (dtb[4]<<24)|(dtb[5]<<16)|(dtb[6]<<8)|dtb[7];
			uint32_t off_strings = (dtb[12]<<24)|(dtb[13]<<16)|(dtb[14]<<8)|dtb[15];
			uint32_t size_strings = (dtb[32]<<24)|(dtb[33]<<16)|(dtb[34]<<8)|dtb[35];

			/* Find "linux,initrd-end" in strings block */
			const char *target = "linux,initrd-end";
			int target_len = 17; /* including NUL */
			int str_off = -1;
			for (uint32_t i = 0; i + target_len <= size_strings; i++) {
				if (memcmp(dtb + off_strings + i, target, target_len) == 0) {
					str_off = i;
					break;
				}
			}

			if (str_off >= 0) {
				/* Scan struct block for FDT_PROP (0x00000003) with this nameoff.
				 * FDT_PROP format: [token:4][len:4][nameoff:4][value:len] */
				uint32_t off_structs = (dtb[8]<<24)|(dtb[9]<<16)|(dtb[10]<<8)|dtb[11];
				for (uint32_t p = off_structs; p + 12 <= totalsize; ) {
					uint32_t tok = (dtb[p]<<24)|(dtb[p+1]<<16)|(dtb[p+2]<<8)|dtb[p+3];
					if (tok == 0x00000003) { /* FDT_PROP */
						uint32_t plen = (dtb[p+4]<<24)|(dtb[p+5]<<16)|(dtb[p+6]<<8)|dtb[p+7];
						uint32_t nameoff = (dtb[p+8]<<24)|(dtb[p+9]<<16)|(dtb[p+10]<<8)|dtb[p+11];
						if ((int)nameoff == str_off && plen == 8) {
							/* Patch the 8-byte value (two 32-bit cells, big-endian) */
							uint32_t hi = (uint32_t)(initrd_end_addr >> 32);
							uint32_t lo = (uint32_t)(initrd_end_addr & 0xFFFFFFFF);
							dtb[p+12] = (hi>>24)&0xff; dtb[p+13] = (hi>>16)&0xff;
							dtb[p+14] = (hi>>8)&0xff;  dtb[p+15] = hi&0xff;
							dtb[p+16] = (lo>>24)&0xff; dtb[p+17] = (lo>>16)&0xff;
							dtb[p+18] = (lo>>8)&0xff;  dtb[p+19] = lo&0xff;
							break;
						}
						/* Skip: token(4) + len(4) + nameoff(4) + value(aligned) */
						p += 12 + ((plen + 3) & ~3u);
					} else {
						p += 4;
					}
				}
			}
		}
	}

	/* Load disk image */
	if (disk_path) {
		int fd = open(disk_path, O_RDONLY);
		if (fd < 0) {
			MemoryContextSwitchTo(old);
			elog(ERROR, "vm_boot: cannot open disk %s: %m", disk_path);
		}
		struct stat st;
		fstat(fd, &st);
		vm_disk_size = st.st_size;
		elog(LOG, "vm_boot: disk %s: %lu bytes, fd=%d",
		     disk_path, (unsigned long)vm_disk_size, fd);
		vm_disk = malloc(vm_disk_size);
		if (!vm_disk) {
			close(fd);
			MemoryContextSwitchTo(old);
			elog(ERROR, "vm_boot: malloc(%lu) failed", (unsigned long)vm_disk_size);
		}
		/* Read in 1GB chunks — macOS read() can fail on >2GB requests */
		uint64_t total_read = 0;
		while (total_read < vm_disk_size) {
			uint64_t want = vm_disk_size - total_read;
			if (want > (1ULL << 30))
				want = (1ULL << 30);
			ssize_t n = read(fd, vm_disk + total_read, (size_t)want);
			if (n <= 0) {
				elog(LOG, "vm_boot: read()=%ld at off=%lu errno=%d(%m)",
				     (long)n, (unsigned long)total_read);
				break;
			}
			total_read += n;
		}
		close(fd);
		elog(LOG, "vm_boot: disk loaded %lu/%lu bytes",
		     (unsigned long)total_read, (unsigned long)vm_disk_size);

		/* Allocate dirty-block bitmap (1 bit per 4KB, zero = clean) */
		uint64_t nblocks = (vm_disk_size + DISK_BLOCK_SIZE - 1) / DISK_BLOCK_SIZE;
		uint64_t bmap_bytes = (nblocks + 7) / 8;
		vm_disk_dirty = calloc(bmap_bytes, 1);
		if (!vm_disk_dirty)
			elog(WARNING, "vm_boot: dirty bitmap alloc failed, disk writes won't be tracked");

		/*
		 * Overlay persisted blocks from vm_blocks table on top of rootfs.img.
		 * This makes the PostgreSQL-stored blocks authoritative after the first
		 * vm_disk_flush(). If vm_blocks is empty (first boot), this is a no-op.
		 */
		if (SPI_connect() == SPI_OK_CONNECT) {
			int spi_rc = SPI_execute(
				"SELECT blk_idx, data FROM vm_blocks WHERE vm_id = 1 ORDER BY blk_idx",
				true, 0);
			if (spi_rc == SPI_OK_SELECT && SPI_processed > 0) {
				uint64_t overlaid = 0;
				for (uint64_t r = 0; r < (uint64_t)SPI_processed; r++) {
					bool isnull;
					Datum d = SPI_getbinval(SPI_tuptable->vals[r],
					                        SPI_tuptable->tupdesc, 1, &isnull);
					if (isnull) continue;
					uint64_t blk = (uint64_t)DatumGetInt64(d);
					d = SPI_getbinval(SPI_tuptable->vals[r],
					                  SPI_tuptable->tupdesc, 2, &isnull);
					if (isnull) continue;
					bytea *bdata = DatumGetByteaP(d);
					uint64_t blen = VARSIZE_ANY_EXHDR(bdata);
					uint64_t off = blk * DISK_BLOCK_SIZE;
					if (off + blen <= vm_disk_size) {
						memcpy(vm_disk + off, VARDATA_ANY(bdata), blen);
						overlaid++;
					}
				}
				if (overlaid > 0)
					elog(LOG, "vm_boot: overlaid %lu persisted blocks from vm_blocks",
					     (unsigned long)overlaid);
			}
			SPI_finish();
		}
	}

	/* Initialize CPU */
	vm_cpu = palloc0(sizeof(struct rv64_cpu));
	rv64_init(vm_cpu, vm_ram, ram_size);
	vm_cpu->uart.tx_callback = pg_uart_tx;

	/* Anchor the real-time clock */
	set_boot_time_ns();

	/* Initialize virtio-blk */
	if (vm_disk) {
		virtio_blk_init(&vm_cpu->virtio_blk, VIRTIO_BLK_IRQ,
		                vm_disk, vm_disk_size);
		/* Hook dirty tracking — no-op if vm_disk_dirty wasn't allocated */
		vm_cpu->virtio_blk.mark_dirty = vm_dirty_callback;
		vm_cpu->virtio_blk.mark_dirty_opaque = NULL;
	}

	//virtio_net_init(&vm_cpu->virtio_net, VIRTIO_NET_IRQ, vm_ram, ram_size);
	vm_cpu->virtio_devs[0] = &vm_cpu->virtio_blk;
	//vm_cpu->virtio_devs[1] = &vm_cpu->virtio_net;

	/* OpenSBI boot protocol */
	vm_cpu->x[10] = 0;          /* a0 = hartid */
	vm_cpu->x[11] = 0x82000000; /* a1 = DTB */

	MemoryContextSwitchTo(old);

	char result[256];
	snprintf(result, sizeof(result),
	         "VM initialized: fw=%ld kern=%ld dtb=%ld initrd=%ld disk=%lu",
	         fw_sz, kern_sz, dtb_sz, init_sz,
	         (unsigned long)vm_disk_size);

	pfree(fw_path); pfree(kern_path); pfree(dtb_path); pfree(initrd_path);
	if (disk_path) pfree(disk_path);

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

/* Helper to load an asset from vm_assets table into RAM */
static long load_asset_from_db(uint8_t *ram, uint64_t ram_size, uint64_t load_addr, const char *asset_type)
{
	long result = -1;
	StringInfoData query;
	initStringInfo(&query);
	appendStringInfo(&query, "SELECT data_oid, size_bytes FROM vm_assets WHERE asset_type = '%s'", asset_type);
	
	Oid data_oid = InvalidOid;
	long size = 0;

	if (SPI_connect() == SPI_OK_CONNECT) {
		int rc = SPI_execute(query.data, true, 1);
		if (rc == SPI_OK_SELECT && SPI_processed > 0) {
			bool isnull;
			Datum d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
			if (!isnull) {
				data_oid = DatumGetObjectId(d);
				d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull);
				size = (long)DatumGetInt64(d);
			}
		} else {
			elog(LOG, "load_asset_from_db('%s'): SPI_execute returned %d, processed %d", asset_type, rc, (int)SPI_processed);
		}
		SPI_finish();
	} else {
		elog(LOG, "load_asset_from_db('%s'): SPI_connect failed", asset_type);
	}
	pfree(query.data);

	if (OidIsValid(data_oid)) {
		uint64_t offset = load_addr - RAM_BASE;
		if (offset + size <= ram_size) {
			if (lo_read_buf(data_oid, ram + offset, size) == size) {
				result = size;
			} else {
				elog(LOG, "load_asset_from_db('%s'): lo_read_buf failed for oid %u", asset_type, data_oid);
			}
		} else {
			elog(LOG, "load_asset_from_db('%s'): size %ld exceeds ram_size %lu", asset_type, size, (unsigned long)ram_size);
		}
	} else {
		elog(LOG, "load_asset_from_db('%s'): Invalid data_oid", asset_type);
	}
	return result;
}

/* Helper to load disk from vm_assets */
static long load_disk_from_db(uint8_t **disk_ptr)
{
	long result = -1;
	*disk_ptr = NULL;
	StringInfoData query;
	initStringInfo(&query);
	appendStringInfo(&query, "SELECT data_oid, size_bytes FROM vm_assets WHERE asset_type = 'disk'");
	
	Oid data_oid = InvalidOid;
	long size = 0;

	if (SPI_connect() == SPI_OK_CONNECT) {
		if (SPI_execute(query.data, true, 1) == SPI_OK_SELECT && SPI_processed > 0) {
			bool isnull;
			Datum d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
			if (!isnull) {
				data_oid = DatumGetObjectId(d);
				d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull);
				size = (long)DatumGetInt64(d);
			}
		}
		SPI_finish();
	}
	pfree(query.data);

	if (OidIsValid(data_oid)) {
		*disk_ptr = malloc(size);
		if (*disk_ptr) {
			if (lo_read_buf(data_oid, *disk_ptr, size) == size) {
				result = size;
			} else {
				free(*disk_ptr);
				*disk_ptr = NULL;
			}
		}
	}
	return result;
}

static void worker_handle_cmd_boot(SharedVMState *st)
{
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	
	int64 vm_id = st->cmd_arg;

	MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

	/* Clean up previous DSM if left dangling */
	if (vm_ram_dsm) {
		dsm_unpin_mapping(vm_ram_dsm);
		dsm_detach(vm_ram_dsm);
		vm_ram_dsm = NULL;
	}

	uint64_t ram_size = (uint64_t)2048 * 1024 * 1024;
	
	vm_ram_dsm = dsm_create(ram_size, 0);
	if (!vm_ram_dsm) {
		snprintf(st->error_msg, sizeof(st->error_msg), "Failed to allocate DSM for vm_ram in vm_boot");
		st->cmd_result = -1;
		PopActiveSnapshot();
		CommitTransactionCommand();
		return;
	}
	dsm_pin_mapping(vm_ram_dsm);
	dsm_pin_segment(vm_ram_dsm); /* Keep segment alive after worker detach */
	vm_ram = dsm_segment_address(vm_ram_dsm);
	memset(vm_ram, 0, ram_size);

	/* Export handle so frontend can attach to this RAM segment */
	st->ram_dsm_handle = dsm_segment_handle(vm_ram_dsm);
	st->ram_size = ram_size;
	st->ram_handle_valid = true;
	
	vm_shared_state->console_pos = 0;

	long disk_sz = -1;
	long fw_sz = -1, kern_sz = -1, dtb_sz = -1, init_sz = -1;

	bool state_restored = false;
	Oid ram_oid = InvalidOid;
	void *cpu_state = NULL;

	if (SPI_connect() == SPI_OK_CONNECT) {
		Oid argtypes[1] = {INT8OID};
		Datum values[1] = {Int64GetDatum((int64_t)vm_id)};
		char nulls[1] = {' '};
		int spi_rc = SPI_execute_with_args(
			"SELECT ram_oid, cpu_state FROM vm_state WHERE id = $1",
			1, argtypes, values, nulls, true, 0);
		if (spi_rc == SPI_OK_SELECT && SPI_processed > 0) {
			bool isnull;
			Datum d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
			if (!isnull)
				ram_oid = DatumGetObjectId(d);
			d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull);
			if (!isnull) {
				bytea *bdata = DatumGetByteaP(d);
				size_t cpu_sz = VARSIZE_ANY_EXHDR(bdata);
				if (cpu_sz == sizeof(struct rv64_cpu)) {
					cpu_state = palloc(cpu_sz);
					memcpy(cpu_state, VARDATA_ANY(bdata), cpu_sz);
				}
			}
		}
		SPI_finish();
	}

	if (OidIsValid(ram_oid)) {
		long read_sz = lo_read_buf(ram_oid, vm_ram, ram_size);
		if (read_sz == (long)ram_size) {
			state_restored = true;
			disk_sz = load_disk_from_db(&vm_disk);
		}
	}

	if (!state_restored) {
		fw_sz   = load_asset_from_db(vm_ram, ram_size, 0x80000000ULL, "firmware");
		kern_sz = load_asset_from_db(vm_ram, ram_size, 0x80200000ULL, "kernel");
		dtb_sz  = load_asset_from_db(vm_ram, ram_size, 0x82000000ULL, "dtb");
		init_sz = load_asset_from_db(vm_ram, ram_size, 0x82200000ULL, "initrd");
		disk_sz = load_disk_from_db(&vm_disk);

		if (fw_sz < 0 || kern_sz < 0 || dtb_sz < 0 || init_sz < 0) {
			MemoryContextSwitchTo(old);
			snprintf(st->error_msg, sizeof(st->error_msg), "Failed to load required boot assets from vm_assets");
			st->cmd_result = -1;
			if (cpu_state) pfree(cpu_state);
			PopActiveSnapshot();
			CommitTransactionCommand();
			return;
		}
	}

	if (disk_sz > 0)
		vm_disk_size = disk_sz;

	/* Patch DTB */
	if (!state_restored) {
		uint8_t *dtb = vm_ram + (0x82000000ULL - RAM_BASE);
		uint64_t initrd_end_addr = 0x82200000ULL + init_sz;
		uint32_t magic = (dtb[0]<<24)|(dtb[1]<<16)|(dtb[2]<<8)|dtb[3];
		if (magic == 0xd00dfeed) {
			uint32_t totalsize = (dtb[4]<<24)|(dtb[5]<<16)|(dtb[6]<<8)|dtb[7];
			uint32_t off_strings = (dtb[12]<<24)|(dtb[13]<<16)|(dtb[14]<<8)|dtb[15];
			uint32_t size_strings = (dtb[32]<<24)|(dtb[33]<<16)|(dtb[34]<<8)|dtb[35];
			const char *target = "linux,initrd-end";
			int target_len = 17;
			int str_off = -1;
			for (uint32_t i = 0; i + target_len <= size_strings; i++) {
				if (memcmp(dtb + off_strings + i, target, target_len) == 0) {
					str_off = i;
					break;
				}
			}
			if (str_off >= 0) {
				uint32_t off_structs = (dtb[8]<<24)|(dtb[9]<<16)|(dtb[10]<<8)|dtb[11];
				for (uint32_t p = off_structs; p + 12 <= totalsize; ) {
					uint32_t tok = (dtb[p]<<24)|(dtb[p+1]<<16)|(dtb[p+2]<<8)|dtb[p+3];
					if (tok == 0x00000003) {
						uint32_t plen = (dtb[p+4]<<24)|(dtb[p+5]<<16)|(dtb[p+6]<<8)|dtb[p+7];
						uint32_t nameoff = (dtb[p+8]<<24)|(dtb[p+9]<<16)|(dtb[p+10]<<8)|dtb[p+11];
						if ((int)nameoff == str_off && plen == 8) {
							uint32_t hi = (uint32_t)(initrd_end_addr >> 32);
							uint32_t lo = (uint32_t)(initrd_end_addr & 0xFFFFFFFF);
							dtb[p+12] = (hi>>24)&0xff; dtb[p+13] = (hi>>16)&0xff;
							dtb[p+14] = (hi>>8)&0xff;  dtb[p+15] = hi&0xff;
							dtb[p+16] = (lo>>24)&0xff; dtb[p+17] = (lo>>16)&0xff;
							dtb[p+18] = (lo>>8)&0xff;  dtb[p+19] = lo&0xff;
							break;
						}
						p += 12 + ((plen + 3) & ~3u);
					} else {
						p += 4;
					}
				}
			}
		}
	}

	/* Initialize CPU */
	vm_cpu = palloc0(sizeof(struct rv64_cpu));
	if (state_restored && cpu_state) {
		memcpy(vm_cpu, cpu_state, sizeof(struct rv64_cpu));
		pfree(cpu_state);
		vm_cpu->jit = NULL;
		memset(vm_cpu->tlb, 0, sizeof(vm_cpu->tlb));
	} else {
		rv64_init(vm_cpu, vm_ram, ram_size);
		if (cpu_state) pfree(cpu_state);
	}
	vm_cpu->uart.tx_callback = pg_uart_tx;
	vm_cpu->id = (uint64_t)vm_id;

	set_boot_time_ns();

	if (vm_disk) {
		uint64_t nblocks = (vm_disk_size + DISK_BLOCK_SIZE - 1) / DISK_BLOCK_SIZE;
		uint64_t bmap_bytes = (nblocks + 7) / 8;
		vm_disk_dirty = calloc(bmap_bytes, 1);
		if (!vm_disk_dirty)
			elog(WARNING, "vm_boot: dirty bitmap alloc failed");

		/* Overlay dirty blocks */
		Oid argtypes[1] = {INT8OID};
		Datum values[1] = {Int64GetDatum((int64_t)vm_id)};
		char nulls[1] = {' '};
		
		if (SPI_connect() == SPI_OK_CONNECT) {
			int spi_rc = SPI_execute_with_args(
				"SELECT blk_idx, data FROM vm_blocks WHERE vm_id = $1 ORDER BY blk_idx",
				1, argtypes, values, nulls, true, 0);
			if (spi_rc == SPI_OK_SELECT && SPI_processed > 0) {
			for (uint64_t r = 0; r < (uint64_t)SPI_processed; r++) {
				bool isnull;
				Datum d = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1, &isnull);
				if (isnull) continue;
				uint64_t blk = (uint64_t)DatumGetInt64(d);
				d = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 2, &isnull);
				if (isnull) continue;
				bytea *bdata = DatumGetByteaP(d);
				uint64_t blen = VARSIZE_ANY_EXHDR(bdata);
				uint64_t off = blk * DISK_BLOCK_SIZE;
				if (off + blen <= vm_disk_size) {
					memcpy(vm_disk + off, VARDATA_ANY(bdata), blen);
				}
			}
			}
			SPI_finish();
		}

		virtio_blk_init(&vm_cpu->virtio_blk, VIRTIO_BLK_IRQ, vm_disk, vm_disk_size);
		vm_cpu->virtio_blk.mark_dirty = vm_dirty_callback;
		vm_cpu->virtio_blk.mark_dirty_opaque = NULL;
	}
	
	virtio_net_init(&vm_cpu->virtio_net, VIRTIO_NET_IRQ, vm_ram, ram_size);
	vm_cpu->virtio_devs[0] = &vm_cpu->virtio_blk;
	vm_cpu->virtio_devs[1] = &vm_cpu->virtio_net;

	vm_cpu->x[10] = 0;
	vm_cpu->x[11] = 0x82000000;

	MemoryContextSwitchTo(old);

	st->cmd_result = 0;
	snprintf(st->error_msg, sizeof(st->error_msg), 
	         "VM initialized from DB: fw=%ld kern=%ld dtb=%ld initrd=%ld disk=%lu",
	         fw_sz, kern_sz, dtb_sz, init_sz, (unsigned long)vm_disk_size);
	         
	PopActiveSnapshot();
	CommitTransactionCommand();
}

static void ipc_send_and_wait(VmIPCCommand cmd, int64_t arg) {
    ensure_worker_attached();
    if (!vm_shared_state || !vm_shared_state->worker_ready)
        elog(ERROR, "VM is not running. Launch background worker first.");

    SpinLockAcquire(&vm_shared_state->mutex);
    vm_shared_state->cmd = cmd;
    vm_shared_state->cmd_arg = arg;
    vm_shared_state->client_latch = MyLatch;
    vm_shared_state->cmd_result = 0;
    SpinLockRelease(&vm_shared_state->mutex);

    SetLatch(vm_shared_state->worker_latch);

    for (;;) {
        ResetLatch(MyLatch);
        CHECK_FOR_INTERRUPTS();
        
        bool done = false;
        SpinLockAcquire(&vm_shared_state->mutex);
        if (vm_shared_state->cmd == VM_CMD_IDLE || vm_shared_state->shutdown_requested) {
            done = true;
        }
        
        /* Native Crash Detection Layer: Validate daemon presence to avoid silent indefinite SQL frontend locks */
        if (!done && vm_shared_state->worker_pid > 0 && kill(vm_shared_state->worker_pid, 0) != 0) {
            vm_shared_state->cmd_result = -1;
            snprintf(vm_shared_state->error_msg, sizeof(vm_shared_state->error_msg), "LinuxSQL Background worker (PID %d) terminated unexpectedly", vm_shared_state->worker_pid);
            done = true;
        }
        SpinLockRelease(&vm_shared_state->mutex);

        if (done) break;

        WaitLatch(MyLatch, WL_LATCH_SET | WL_POSTMASTER_DEATH, -1, PG_WAIT_EXTENSION);
    }
    
    if (vm_shared_state->cmd_result < 0) {
        elog(ERROR, "LinuxSQL Background Worker Error: %s", vm_shared_state->error_msg);
    }
}

static void ipc_send_and_wait_ext(VmIPCCommand cmd, int64_t arg, int64_t arg_ext) {
    ensure_worker_attached();
    if (!vm_shared_state || !vm_shared_state->worker_ready)
        elog(ERROR, "VM is not running. Launch background worker first.");

    SpinLockAcquire(&vm_shared_state->mutex);
    vm_shared_state->cmd = cmd;
    vm_shared_state->cmd_arg = arg;
    vm_shared_state->cmd_arg_ext = arg_ext;
    vm_shared_state->client_latch = MyLatch;
    vm_shared_state->cmd_result = 0;
    SpinLockRelease(&vm_shared_state->mutex);

    SetLatch(vm_shared_state->worker_latch);

    for (;;) {
        ResetLatch(MyLatch);
        CHECK_FOR_INTERRUPTS();
        
        bool done = false;
        SpinLockAcquire(&vm_shared_state->mutex);
        if (vm_shared_state->cmd == VM_CMD_IDLE || vm_shared_state->shutdown_requested) {
            done = true;
        }
        
        /* Native Crash Detection Layer: Validate daemon presence to avoid silent indefinite SQL frontend locks */
        if (!done && vm_shared_state->worker_pid > 0 && kill(vm_shared_state->worker_pid, 0) != 0) {
            vm_shared_state->cmd_result = -1;
            snprintf(vm_shared_state->error_msg, sizeof(vm_shared_state->error_msg), "LinuxSQL Background worker (PID %d) terminated unexpectedly", vm_shared_state->worker_pid);
            done = true;
        }
        SpinLockRelease(&vm_shared_state->mutex);

        if (done) break;

        WaitLatch(MyLatch, WL_LATCH_SET | WL_POSTMASTER_DEATH, -1, PG_WAIT_EXTENSION);
    }
    
    if (vm_shared_state->cmd_result < 0) {
        elog(ERROR, "LinuxSQL Background Worker Error: %s", vm_shared_state->error_msg);
    }
}

static void launch_native_worker(void) {
    ensure_worker_attached();
    if (vm_dsm != NULL && vm_shared_state && vm_shared_state->worker_ready) return; /* Already running */

    if (vm_dsm != NULL) {
        dsm_unpin_mapping(vm_dsm);
        dsm_detach(vm_dsm);
        vm_dsm = NULL;
    }

    vm_dsm = dsm_create(sizeof(SharedVMState), 0);
    if (!vm_dsm) elog(ERROR, "Failed to allocate dynamic shared memory for VM.");

    dsm_pin_mapping(vm_dsm); /* Prevent teardown at end of transaction */
    dsm_pin_segment(vm_dsm); /* Prevent teardown of segment on backend exit */
    
    vm_shared_state = (SharedVMState *)dsm_segment_address(vm_dsm);
    memset(vm_shared_state, 0, sizeof(SharedVMState));
    vm_shared_state->magic = VM_IPC_MAGIC;
    vm_shared_state->jit_block_size_request = -1;  /* no pending change */
    SpinLockInit(&vm_shared_state->mutex);

    BackgroundWorker worker;
    BackgroundWorkerHandle *handle;
    memset(&worker, 0, sizeof(worker));
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    sprintf(worker.bgw_library_name, "linuxsql_vm");
    sprintf(worker.bgw_function_name, "linuxsql_vm_worker_main");
    sprintf(worker.bgw_name, "LinuxSQL VM Executor");
    sprintf(worker.bgw_type, "linuxsql_vm");
    worker.bgw_main_arg = UInt32GetDatum(dsm_segment_handle(vm_dsm));

    if (!RegisterDynamicBackgroundWorker(&worker, &handle)) {
        dsm_detach(vm_dsm);
        vm_dsm = NULL;  vm_shared_state = NULL;
        elog(ERROR, "Failed to launch dynamic background worker for VM.");
    }

    while (!vm_shared_state->worker_ready && !vm_shared_state->shutdown_requested) {
        pg_usleep(10000);
    }

    if (!vm_shared_state->worker_ready) {
        elog(ERROR, "VM Background worker immediately shut down or failed to boot.");
    } else {
        SPI_connect();
        Oid argtypes[2] = { INT8OID, INT4OID };
        Datum values[2];
        char nulls[2] = { ' ', ' ' };
        values[0] = Int64GetDatum((int64_t)dsm_segment_handle(vm_dsm));
        values[1] = Int32GetDatum(handle ? vm_shared_state->worker_pid : -1); /* Handle not strictly needed here since worker writes its pid */
        if (vm_shared_state->worker_pid > 0) values[1] = Int32GetDatum(vm_shared_state->worker_pid);
        
        SPI_execute_with_args("UPDATE vm_state SET vm_dsm_handle = $1, vm_worker_pid = $2 WHERE id = 1",
                              2, argtypes, values, nulls, false, 0);
        SPI_finish();
    }
}

/*
 * vm_boot() → TEXT
 *
 * Initialize the VM by loading fw, kernel, dtb, initrd, and disk
 * exclusively from the vm_assets table. No host filesystem access.
 */
PG_FUNCTION_INFO_V1(rv64_vm_boot_from_db);
Datum
rv64_vm_boot_from_db(PG_FUNCTION_ARGS)
{
	int64 vm_id = 1;
	if (PG_NARGS() > 0 && !PG_ARGISNULL(0))
		vm_id = PG_GETARG_INT64(0);
		
	launch_native_worker();
	ipc_send_and_wait(VM_CMD_BOOT, vm_id);

	char result[256];
	snprintf(result, sizeof(result), "%s", vm_shared_state->error_msg);
	PG_RETURN_TEXT_P(cstring_to_text(result));
}

/*
 * vm_step(cycles BIGINT DEFAULT 10000000) → BIGINT
 *
 * Run the VM for the given number of cycles via the Background Worker.
 * Returns the number of instructions actually executed.
 */
PG_FUNCTION_INFO_V1(rv64_vm_step);
Datum
rv64_vm_step(PG_FUNCTION_ARGS)
{
	int64_t cycles = PG_GETARG_INT64(0);
	if (cycles <= 0) cycles = 10000000;

	ipc_send_and_wait(VM_CMD_STEP, cycles);

	PG_RETURN_INT64((int64_t)vm_shared_state->cmd_return);
}

/*
 * vm_state() → RECORD
 *
 * Returns the full CPU/PLIC/virtio state as a composite row.
 */
PG_FUNCTION_INFO_V1(rv64_vm_state);
Datum
rv64_vm_state(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_state: no VM initialized");
	ensure_frontend_ram();

	TupleDesc tupdesc;
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "vm_state: must be called as composite-returning function");

	tupdesc = BlessTupleDesc(tupdesc);

	/* 21 columns — see the SQL CREATE TYPE definition */
	Datum values[21];
	bool  nulls[21];
	memset(nulls, false, sizeof(nulls));

	struct rv64_cpu *c = &vm_shared_state->cpu;

	/* CPU state */
	values[0]  = Int64GetDatum((int64_t)c->pc);
	values[1]  = Int32GetDatum(c->priv);
	values[2]  = BoolGetDatum(c->halted != 0);
	values[3]  = Int64GetDatum((int64_t)c->mip);
	values[4]  = Int64GetDatum((int64_t)c->mie);
	values[5]  = Int64GetDatum((int64_t)c->mideleg);
	values[6]  = Int64GetDatum((int64_t)c->mstatus);
	values[7]  = Int64GetDatum((int64_t)c->instret);
	values[8]  = Int64GetDatum((int64_t)c->clint.mtime);
	values[9]  = BoolGetDatum(c->error != 0);

	/* PLIC state */
	values[10] = Int32GetDatum((int32_t)c->plic.pending[0]);
	values[11] = Int32GetDatum((int32_t)c->plic.enable[1][0]); /* S-mode */
	values[12] = Int32GetDatum((int32_t)c->plic.claim[1]);
	values[13] = Int32GetDatum((int32_t)c->plic.threshold[1]);

	/* Virtio state */
	values[14] = Int32GetDatum((int32_t)c->virtio_blk.status);
	values[15] = Int32GetDatum((int32_t)c->virtio_blk.int_status);
	values[16] = Int32GetDatum(c->virtio_blk.queues[0].ready);
	values[17] = Int32GetDatum((int32_t)c->virtio_blk.queues[0].last_avail);
	/* Read avail_idx from guest memory */
	uint16_t avail_idx = 0;
	if (c->virtio_blk.queues[0].avail_addr) {
		uint64_t a = c->virtio_blk.queues[0].avail_addr;
		uint8_t *ram = get_guest_ram_ptr();
		if (ram && a >= RAM_BASE && a - RAM_BASE + 4 < c->ram_size) {
			memcpy(&avail_idx, ram + (a - RAM_BASE) + 2, 2);
		}
	}
	values[18] = Int32GetDatum((int32_t)avail_idx);
	/* Read used_idx from guest memory */
	uint16_t used_idx = 0;
	if (c->virtio_blk.queues[0].used_addr) {
		uint64_t u = c->virtio_blk.queues[0].used_addr;
		uint8_t *ram = get_guest_ram_ptr();
		if (ram && u >= RAM_BASE && u - RAM_BASE + 4 < c->ram_size) {
			memcpy(&used_idx, ram + (u - RAM_BASE) + 2, 2);
		}
	}
	values[19] = Int32GetDatum((int32_t)used_idx);
	values[20] = Int32GetDatum(vm_shared_state->console_pos);

	HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * vm_send(input TEXT) → void
 *
 * Send characters to the VM's UART (as if typed on console).
 */
PG_FUNCTION_INFO_V1(rv64_vm_send);
Datum
rv64_vm_send(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_send: no VM initialized");

	char *input = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int len = strlen(input);
	
	/* Copy and append newline if needed */
	strncpy(vm_shared_state->input_buf, input, 1022);
	if (len > 0 && input[len-1] != '\n' && len < 1022) {
	    vm_shared_state->input_buf[len] = '\n';
	    vm_shared_state->input_buf[len+1] = '\0';
	} else {
	    vm_shared_state->input_buf[1023] = '\0';
	}
	
	ipc_send_and_wait(VM_CMD_SEND, 0);

	pfree(input);
	PG_RETURN_VOID();
}

/*
 * vm_console() → TEXT
 *
 * Returns all console output accumulated since boot (or last clear).
 */
PG_FUNCTION_INFO_V1(rv64_vm_console);
Datum
rv64_vm_console(PG_FUNCTION_ARGS)
{
	if (!vm_shared_state || vm_shared_state->console_pos == 0)
		PG_RETURN_TEXT_P(cstring_to_text(""));

	PG_RETURN_TEXT_P(cstring_to_text_with_len(vm_shared_state->console_buf, vm_shared_state->console_pos));
}

/*
 * vm_inst_log() → TEXT
 *
 * Returns recently stringified instructions.
 */
PG_FUNCTION_INFO_V1(rv64_vm_inst_log);
Datum
rv64_vm_inst_log(PG_FUNCTION_ARGS)
{
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_inst_log: no VM initialized");

	struct rv64_cpu *cpu = &vm_shared_state->cpu;

	StringInfoData buf;
	initStringInfo(&buf);
	appendStringInfo(&buf, "Last instructions:\n");

	int count = (cpu->dbg.inst_log_idx < DBG_INST_LOG_SIZE) ? cpu->dbg.inst_log_idx : DBG_INST_LOG_SIZE;
	int start_idx = (cpu->dbg.inst_log_idx < DBG_INST_LOG_SIZE) ? 0 : cpu->dbg.inst_log_idx % DBG_INST_LOG_SIZE;

	for (int i = 0; i < count; i++) {
		int idx = (start_idx + i) % DBG_INST_LOG_SIZE;
		appendStringInfo(&buf, "PC=0x%lx insn=0x%08x instret=%lu\n",
			(unsigned long)cpu->dbg.inst_log[idx].pc,
			cpu->dbg.inst_log[idx].insn,
			(unsigned long)cpu->dbg.inst_log[idx].instret);
	}

	PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}


/*
 * vm_console_tail(n INT) → TEXT
 *
 * Returns the last n characters of console output.
 */
PG_FUNCTION_INFO_V1(rv64_vm_console_tail);
Datum
rv64_vm_console_tail(PG_FUNCTION_ARGS)
{
	if (!vm_shared_state || vm_shared_state->console_pos == 0)
		PG_RETURN_TEXT_P(cstring_to_text(""));

	int n = PG_GETARG_INT32(0);
	if (n <= 0 || n >= vm_shared_state->console_pos)
		PG_RETURN_TEXT_P(cstring_to_text_with_len(vm_shared_state->console_buf, vm_shared_state->console_pos));

	PG_RETURN_TEXT_P(cstring_to_text_with_len(vm_shared_state->console_buf + vm_shared_state->console_pos - n, n));
}

/*
 * vm_perf(cycles BIGINT DEFAULT 100000000) → RECORD(wall_ms, instructions, mips)
 *
 * Run the VM for N cycles and measure wall-clock time.
 */
#include <time.h>

PG_FUNCTION_INFO_V1(rv64_vm_perf);
Datum
rv64_vm_perf(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_perf: no VM initialized");

	int64_t cycles = PG_GETARG_INT64(0);
	if (cycles <= 0) cycles = 100000000;

	TupleDesc tupdesc;
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "vm_perf: must be called as composite-returning function");
	tupdesc = BlessTupleDesc(tupdesc);

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	uint64_t before = vm_shared_state->cpu.instret;
	ipc_send_and_wait(VM_CMD_STEP, cycles);
	uint64_t insns = vm_shared_state->cpu.instret - before;

	clock_gettime(CLOCK_MONOTONIC, &t1);
	double wall_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
	                 (t1.tv_nsec - t0.tv_nsec) / 1e6;
	double mips = (wall_ms > 0.001) ? (insns / 1000.0) / wall_ms : 0;

	Datum values[3];
	bool nulls[3] = {false, false, false};
	values[0] = Float8GetDatum(wall_ms);
	values[1] = Int64GetDatum((int64_t)insns);
	values[2] = Float8GetDatum(mips);

	HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * vm_profile() → RECORD (time breakdown + interrupt stats + MMU stats)
 *
 * Returns accumulated profiling counters since boot.
 */
PG_FUNCTION_INFO_V1(rv64_vm_profile);
Datum
rv64_vm_profile(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_profile: no VM initialized");

	TupleDesc tupdesc;
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "vm_profile: must be called as composite-returning function");
	tupdesc = BlessTupleDesc(tupdesc);

	Datum values[8];
	bool nulls[8];
	memset(nulls, false, sizeof(nulls));

	values[0] = Int64GetDatum((int64_t)vm_shared_state->cpu.prof.exec_cycles);
	values[1] = Int64GetDatum((int64_t)vm_shared_state->cpu.prof.wfi_cycles);
	values[2] = Int64GetDatum((int64_t)vm_shared_state->cpu.prof.irq_checks);
	values[3] = Int64GetDatum((int64_t)vm_shared_state->cpu.prof.irq_taken);
	values[4] = Int64GetDatum((int64_t)vm_shared_state->cpu.prof.mmu_walks);
	values[5] = Int64GetDatum((int64_t)vm_shared_state->cpu.prof.mmu_walk_steps);
	values[6] = Int64GetDatum((int64_t)vm_shared_state->cpu.prof.tlb_hits);
	values[7] = Int64GetDatum((int64_t)vm_shared_state->cpu.prof.tlb_misses);

	HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * vm_insn_stats() → RECORD (instruction class distribution)
 */
PG_FUNCTION_INFO_V1(rv64_vm_insn_stats);
Datum
rv64_vm_insn_stats(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_insn_stats: no VM initialized");

	TupleDesc tupdesc;
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "vm_insn_stats: must be called as composite-returning function");
	tupdesc = BlessTupleDesc(tupdesc);

	Datum values[10];
	bool nulls[10];
	memset(nulls, false, sizeof(nulls));

	values[0] = Int64GetDatum((int64_t)vm_shared_state->cpu.prof.insn_alu);
	values[1] = Int64GetDatum((int64_t)vm_shared_state->cpu.prof.insn_load);
	values[2] = Int64GetDatum((int64_t)vm_shared_state->cpu.prof.insn_store);
	values[3] = Int64GetDatum((int64_t)vm_shared_state->cpu.prof.insn_branch);
	values[4] = Int64GetDatum((int64_t)vm_shared_state->cpu.prof.insn_jump);
	values[5] = Int64GetDatum((int64_t)vm_shared_state->cpu.prof.insn_csr);
	values[6] = Int64GetDatum((int64_t)vm_shared_state->cpu.prof.insn_fpu);
	values[7] = Int64GetDatum((int64_t)vm_shared_state->cpu.prof.insn_atomic);
	values[8] = Int64GetDatum((int64_t)vm_shared_state->cpu.prof.insn_system);
	values[9] = Int64GetDatum((int64_t)vm_shared_state->cpu.prof.insn_other);

	HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * vm_io_stats() → RECORD (virtio block I/O counters)
 */
PG_FUNCTION_INFO_V1(rv64_vm_io_stats);
Datum
rv64_vm_io_stats(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_io_stats: no VM initialized");

	TupleDesc tupdesc;
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "vm_io_stats: must be called as composite-returning function");
	tupdesc = BlessTupleDesc(tupdesc);

	Datum values[4];
	bool nulls[4];
	memset(nulls, false, sizeof(nulls));

	values[0] = Int64GetDatum((int64_t)vm_shared_state->cpu.virtio_blk.blk_reads);
	values[1] = Int64GetDatum((int64_t)vm_shared_state->cpu.virtio_blk.blk_writes);
	values[2] = Int64GetDatum((int64_t)vm_shared_state->cpu.virtio_blk.blk_bytes_read);
	values[3] = Int64GetDatum((int64_t)vm_shared_state->cpu.virtio_blk.blk_bytes_written);

	HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * vm_jit_stats() → RECORD (JIT compiler diagnostic counters)
 */
PG_FUNCTION_INFO_V1(rv64_vm_jit_stats);
Datum
rv64_vm_jit_stats(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_jit_stats: no VM initialized");

	TupleDesc tupdesc;
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "vm_jit_stats: must be called as composite-returning function");
	tupdesc = BlessTupleDesc(tupdesc);

	Datum values[6];
	bool nulls[6];
	memset(nulls, false, sizeof(nulls));

	/* jit pointer is worker-local (malloc'd), can't dereference from frontend.
	 * Use non-NULL as a boolean "JIT was enabled" check only.
	 * Actual counters live in struct jit_state which is inaccessible here. */
	if (vm_shared_state->cpu.jit) {
		values[0] = BoolGetDatum(true);
		values[1] = Int64GetDatum(0);  /* TODO: sync jit stats into prof struct */
		values[2] = Int64GetDatum(0);
		values[3] = Int64GetDatum(0);
		values[4] = Int64GetDatum(0);
		values[5] = Float8GetDatum(0.0);
	} else {
		values[0] = BoolGetDatum(false);
		values[1] = Int64GetDatum(0);
		values[2] = Int64GetDatum(0);
		values[3] = Int64GetDatum(0);
		values[4] = Int64GetDatum(0);
		values[5] = Float8GetDatum(0.0);
	}

	HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/* ================================================================
 * Console buffer management
 * ================================================================ */

/*
 * vm_console_clear() → VOID
 *
 * Reset the console output buffer.
 */
PG_FUNCTION_INFO_V1(rv64_vm_console_clear);
Datum
rv64_vm_console_clear(PG_FUNCTION_ARGS)
{
	if (vm_shared_state) {
		vm_shared_state->console_pos = 0;
		vm_shared_state->console_buf[0] = '\0';
	}
	PG_RETURN_VOID();
}

/*
 * vm_type(text TEXT) → VOID
 *
 * Feed characters into the UART RX buffer.
 */
extern void rv64_uart_rx(struct rv64_cpu *cpu, uint8_t ch);
PG_FUNCTION_INFO_V1(rv64_vm_type);
Datum
rv64_vm_type(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_type: no VM initialized");

	text *t = PG_GETARG_TEXT_PP(0);
	int len = VARSIZE_ANY_EXHDR(t);
	char *str = VARDATA_ANY(t);

	/* Feed raw characters via IPC — vm_cpu is NULL in the frontend */
	if (len > 1023) len = 1023;
	memcpy(vm_shared_state->input_buf, str, len);
	vm_shared_state->input_buf[len] = '\0';

	ipc_send_and_wait(VM_CMD_SEND, 0);

	PG_RETURN_VOID();
}

/* ================================================================
 * Debug Instrumentation — SQL-callable watchpoints, breakpoints,
 * memory reads, and register dumps.
 * ================================================================ */

/*
 * vm_peek(addr BIGINT, size INT DEFAULT 8) → BIGINT
 *
 * Read `size` bytes (1/2/4/8) from guest virtual memory at `addr`.
 * Returns the value as a BIGINT. Does page table translation.
 */
PG_FUNCTION_INFO_V1(rv64_vm_peek);
Datum
rv64_vm_peek(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_peek: no VM initialized");
	ensure_frontend_ram();

	uint64_t addr = (uint64_t)PG_GETARG_INT64(0);
	int size = PG_ARGISNULL(1) ? 8 : PG_GETARG_INT32(1);

	if (size != 1 && size != 2 && size != 4 && size != 8)
		elog(ERROR, "vm_peek: size must be 1, 2, 4, or 8");

	uint64_t val = 0;
	int exc = rv64_jit_load(&vm_shared_state->cpu, addr, size, &val);
	if (exc)
		elog(ERROR, "vm_peek: load fault at 0x%lx (exc %d)",
		     (unsigned long)addr, exc);

	PG_RETURN_INT64((int64_t)val);
}

/*
 * vm_poke(addr BIGINT, value BIGINT, size INT DEFAULT 8) → VOID
 *
 * Write `size` bytes to guest virtual memory. For patching guest state.
 */
PG_FUNCTION_INFO_V1(rv64_vm_poke);
Datum
rv64_vm_poke(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_poke: no VM initialized");
	ensure_frontend_ram();

	uint64_t addr = (uint64_t)PG_GETARG_INT64(0);
	uint64_t val = (uint64_t)PG_GETARG_INT64(1);
	int size = PG_ARGISNULL(2) ? 8 : PG_GETARG_INT32(2);

	if (size != 1 && size != 2 && size != 4 && size != 8)
		elog(ERROR, "vm_poke: size must be 1, 2, 4, or 8");

	int exc = rv64_jit_store(&vm_shared_state->cpu, addr, size, val);
	if (exc)
		elog(ERROR, "vm_poke: store fault at 0x%lx (exc %d)",
		     (unsigned long)addr, exc);

	PG_RETURN_VOID();
}

/*
 * vm_watchpoint(addr BIGINT, size INT DEFAULT 4) → TEXT
 *
 * Set a memory watchpoint: log every store that touches
 * [addr, addr+size). Pass addr=0 to disable.
 * Clears previous log entries.
 */
PG_FUNCTION_INFO_V1(rv64_vm_watchpoint);
Datum
rv64_vm_watchpoint(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_watchpoint: no VM initialized");

	uint64_t addr = (uint64_t)PG_GETARG_INT64(0);
	int size = PG_ARGISNULL(1) ? 4 : PG_GETARG_INT32(1);

	ipc_send_and_wait_ext(VM_CMD_WATCHPOINT, (int64_t)addr, (int64_t)size);

	char buf[128];
	if (addr)
		snprintf(buf, sizeof(buf), "watchpoint armed: 0x%lx [%d bytes]",
		         (unsigned long)addr, size);
	else
		snprintf(buf, sizeof(buf), "watchpoint disabled");

	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

/*
 * vm_breakpoint(pc BIGINT) → TEXT
 *
 * Set a PC breakpoint: snapshot registers whenever execution reaches
 * the given PC. Pass pc=0 to disable. Clears previous log entries.
 */
PG_FUNCTION_INFO_V1(rv64_vm_breakpoint);
Datum
rv64_vm_breakpoint(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_breakpoint: no VM initialized");

	uint64_t pc = (uint64_t)PG_GETARG_INT64(0);
	/* Optional 2nd arg: halt on hit (default false) */
	bool halt = PG_NARGS() > 1 && !PG_ARGISNULL(1) && PG_GETARG_BOOL(1);

	ipc_send_and_wait_ext(VM_CMD_BREAKPOINT, (int64_t)pc, halt ? 1 : 0);

	char buf[128];
	if (pc)
		snprintf(buf, sizeof(buf), "breakpoint armed: 0x%lx", (unsigned long)pc);
	else
		snprintf(buf, sizeof(buf), "breakpoint disabled");

	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

/*
 * vm_watch_log() → SETOF RECORD(hit_num, pc, addr, value, size, instret)
 *
 * Return all watchpoint hits from the ring buffer.
 */
PG_FUNCTION_INFO_V1(rv64_vm_watch_log);
Datum
rv64_vm_watch_log(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	int total_hits;

	if (SRF_IS_FIRSTCALL()) {
		funcctx = SRF_FIRSTCALL_INIT();
		ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
			SRF_RETURN_DONE(funcctx);

		MemoryContext old = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		total_hits = vm_shared_state->cpu.dbg.store_log_count;
		if (total_hits > DBG_STORE_LOG_SIZE)
			total_hits = DBG_STORE_LOG_SIZE;
		funcctx->max_calls = total_hits;

		TupleDesc tupdesc = CreateTemplateTupleDesc(6);
		TupleDescInitEntry(tupdesc, 1, "hit_num", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, 2, "pc", INT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, 3, "addr", INT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, 4, "value", INT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, 5, "size", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, 6, "instret", INT8OID, -1, 0);
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		MemoryContextSwitchTo(old);
	}

	funcctx = SRF_PERCALL_SETUP();

	if (funcctx->call_cntr < funcctx->max_calls) {
		/* Walk ring buffer in order: oldest first */
		int total = vm_shared_state->cpu.dbg.store_log_count;
		int start = (total > DBG_STORE_LOG_SIZE)
			? total % DBG_STORE_LOG_SIZE : 0;
		int idx = (start + funcctx->call_cntr) % DBG_STORE_LOG_SIZE;

		Datum vals[6];
		bool nulls[6] = {false};
		vals[0] = Int32GetDatum((int32_t)(total > DBG_STORE_LOG_SIZE
			? total - DBG_STORE_LOG_SIZE + funcctx->call_cntr
			: funcctx->call_cntr));
		vals[1] = Int64GetDatum((int64_t)vm_shared_state->cpu.dbg.store_log[idx].pc);
		vals[2] = Int64GetDatum((int64_t)vm_shared_state->cpu.dbg.store_log[idx].addr);
		vals[3] = Int64GetDatum((int64_t)vm_shared_state->cpu.dbg.store_log[idx].value);
		vals[4] = Int32GetDatum((int32_t)vm_shared_state->cpu.dbg.store_log[idx].size);
		vals[5] = Int64GetDatum((int64_t)vm_shared_state->cpu.dbg.store_log[idx].instret);

		HeapTuple tuple = heap_form_tuple(funcctx->tuple_desc, vals, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * vm_break_log() → SETOF RECORD(hit_num, pc, sp, instret, x_hex, f_hex)
 *
 * Return breakpoint snapshots. x_hex and f_hex are hex-encoded register dumps.
 */
PG_FUNCTION_INFO_V1(rv64_vm_break_log);
Datum
rv64_vm_break_log(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;

	if (SRF_IS_FIRSTCALL()) {
		funcctx = SRF_FIRSTCALL_INIT();
		ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
			SRF_RETURN_DONE(funcctx);

		MemoryContext old = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		int total = vm_shared_state->cpu.dbg.break_log_count;
		if (total > DBG_BREAK_LOG_SIZE) total = DBG_BREAK_LOG_SIZE;
		funcctx->max_calls = total;

		TupleDesc tupdesc = CreateTemplateTupleDesc(6);
		TupleDescInitEntry(tupdesc, 1, "hit_num", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, 2, "pc", INT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, 3, "sp", INT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, 4, "instret", INT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, 5, "x_hex", TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, 6, "f_hex", TEXTOID, -1, 0);
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		MemoryContextSwitchTo(old);
	}

	funcctx = SRF_PERCALL_SETUP();

	if (funcctx->call_cntr < funcctx->max_calls) {
		int total = vm_shared_state->cpu.dbg.break_log_count;
		int start = (total > DBG_BREAK_LOG_SIZE)
			? total % DBG_BREAK_LOG_SIZE : 0;
		int idx = (start + funcctx->call_cntr) % DBG_BREAK_LOG_SIZE;

		/* Encode integer registers as hex string */
		char x_hex[32 * 17 + 1]; /* 32 regs × "XXXXXXXXXXXXXXXX " */
		for (int i = 0; i < 32; i++)
			snprintf(x_hex + i * 17, 18, "%016lx ",
			         (unsigned long)vm_shared_state->cpu.dbg.break_log[idx].x[i]);
		x_hex[32 * 17 - 1] = '\0';

		/* Encode FP registers as hex string */
		char f_hex[32 * 17 + 1];
		for (int i = 0; i < 32; i++)
			snprintf(f_hex + i * 17, 18, "%016lx ",
			         (unsigned long)vm_shared_state->cpu.dbg.break_log[idx].f[i]);
		f_hex[32 * 17 - 1] = '\0';

		Datum vals[6];
		bool nulls[6] = {false};
		vals[0] = Int32GetDatum((int32_t)funcctx->call_cntr);
		vals[1] = Int64GetDatum((int64_t)vm_shared_state->cpu.dbg.break_log[idx].pc);
		vals[2] = Int64GetDatum((int64_t)vm_shared_state->cpu.dbg.break_log[idx].sp);
		vals[3] = Int64GetDatum((int64_t)vm_shared_state->cpu.dbg.break_log[idx].instret);
		vals[4] = CStringGetTextDatum(x_hex);
		vals[5] = CStringGetTextDatum(f_hex);

		HeapTuple tuple = heap_form_tuple(funcctx->tuple_desc, vals, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * vm_regs() → RECORD (current register state snapshot)
 *
 * Returns PC, SP, all 32 integer regs, all 32 FP regs as hex strings.
 */
PG_FUNCTION_INFO_V1(rv64_vm_regs);
Datum
rv64_vm_regs(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_regs: no VM initialized");

	TupleDesc tupdesc;
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "vm_regs: must be called as composite-returning function");
	tupdesc = BlessTupleDesc(tupdesc);

	/* Encode integer registers */
	char x_hex[32 * 17 + 1];
	for (int i = 0; i < 32; i++)
		snprintf(x_hex + i * 17, 18, "%016lx ",
		         (unsigned long)vm_shared_state->cpu.x[i]);
	x_hex[32 * 17 - 1] = '\0';

	/* Encode FP registers */
	char f_hex[32 * 17 + 1];
	for (int i = 0; i < 32; i++)
		snprintf(f_hex + i * 17, 18, "%016lx ",
		         (unsigned long)vm_shared_state->cpu.f[i]);
	f_hex[32 * 17 - 1] = '\0';

	Datum values[5];
	bool nulls[5] = {false};
	values[0] = Int64GetDatum((int64_t)vm_shared_state->cpu.pc);
	values[1] = Int64GetDatum((int64_t)vm_shared_state->cpu.x[2]); /* sp */
	values[2] = Int64GetDatum((int64_t)vm_shared_state->cpu.instret);
	values[3] = CStringGetTextDatum(x_hex);
	values[4] = CStringGetTextDatum(f_hex);

	HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * vm_find(pattern BYTEA, start_addr BIGINT, length INT) → BIGINT or NULL
 *
 * Scan guest virtual memory for the first occurrence of `pattern`.
 * Returns the address of the match, or NULL if not found.
 */
PG_FUNCTION_INFO_V1(rv64_vm_find);
Datum
rv64_vm_find(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_find: no VM initialized");
	ensure_frontend_ram();

	bytea *pattern = PG_GETARG_BYTEA_PP(0);
	uint64_t start = (uint64_t)PG_GETARG_INT64(1);
	int length = PG_GETARG_INT32(2);

	int pat_len = VARSIZE_ANY_EXHDR(pattern);
	uint8_t *pat = (uint8_t *)VARDATA_ANY(pattern);

	if (pat_len <= 0 || pat_len > length)
		PG_RETURN_NULL();

	int match_pos = 0;
	for (int i = 0; i < length; i++) {
		uint64_t val = 0;
		int exc = rv64_jit_load(&vm_shared_state->cpu, start + i, 1, &val);
		if (exc) {
			match_pos = 0;
			continue;
		}

		if ((uint8_t)val == pat[match_pos]) {
			match_pos++;
			if (match_pos == pat_len) {
				uint64_t found_addr = start + i - pat_len + 1;
				PG_RETURN_INT64((int64_t)found_addr);
			}
		} else {
			if (match_pos > 0) {
				i -= match_pos;
				match_pos = 0;
			}
		}
	}

	PG_RETURN_NULL();
}

/*
 * vm_resume() → TEXT
 *
 * Clear the debug halt flag, allowing vm_step() to run again.
 */
PG_FUNCTION_INFO_V1(rv64_vm_resume);
Datum
rv64_vm_resume(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_resume: no VM initialized");

	vm_shared_state->cpu.dbg.debug_halt = 0;
	PG_RETURN_TEXT_P(cstring_to_text("VM resumed"));
}

/*
 * vm_step_until(pattern TEXT, max_cycles BIGINT) → RECORD
 *
 * Run the VM until `pattern` appears in NEW console output,
 * or max_cycles is exhausted. Sets debug_halt=1 when found/stopped.
 */
PG_FUNCTION_INFO_V1(rv64_vm_step_until);
Datum
rv64_vm_step_until(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_step_until: no VM initialized via background worker");

	char *pattern = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int64_t max_cycles = PG_ARGISNULL(1) ? 500000000LL : PG_GETARG_INT64(1);
	int pat_len = strlen(pattern);

	TupleDesc tupdesc;
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "vm_step_until: must be called as composite-returning function");
	tupdesc = BlessTupleDesc(tupdesc);

	/* Parallel JIT worker disabled — JIT compilation is synchronous inline.
	 * This infrastructure remains for future async I/O offloading. */
	/* Worker checks shared debug flags if needed, but we check pattern synchronously here */
	int search_from = vm_shared_state->console_pos;
	int64_t batch = 50000000; /* 50M cycles per batch */
	uint64_t start_instret = vm_shared_state->cpu.instret;
	int64_t max_iterations = max_cycles / batch + 1;
	int64_t iterations = 0;
	bool found = false;

	while (iterations < max_iterations && !vm_shared_state->cpu.error) {
		elog(WARNING, "DEBUG: starting iteration %ld", iterations);
		/* Delegate stepping to the Background Worker instead of running inline */
		ipc_send_and_wait(VM_CMD_STEP, batch);
		elog(WARNING, "DEBUG: returned from ipc_send_and_wait on iteration %ld", iterations);
		iterations++;

		if (iterations % 20 == 0) {
			elog(NOTICE, "VM running: %ld billion cycles, instret=%lu...", (iterations * batch) / 1000000000, vm_shared_state->cpu.instret);
		}

		if (vm_shared_state->cpu.dbg.debug_halt)
			break;

		if (vm_shared_state->console_pos > search_from && pat_len > 0) {
			int scan = search_from;
			if (scan >= pat_len) scan -= pat_len;
			for (int i = scan; i <= vm_shared_state->console_pos - pat_len; i++) {
				if (memcmp(vm_shared_state->console_buf + i, pattern, pat_len) == 0) {
					found = true;
					vm_shared_state->cpu.dbg.debug_halt = 1;
					break;
				}
			}
			search_from = vm_shared_state->console_pos;
			if (found) break;
		}
		elog(WARNING, "DEBUG: completed string matching for iteration %ld", iterations);
	}
	
	elog(WARNING, "DEBUG: reached end of loop, formatting tuples");

	int snippet_len = vm_shared_state->console_pos > 200 ? 200 : vm_shared_state->console_pos;
	int snippet_start = vm_shared_state->console_pos > snippet_len
		? vm_shared_state->console_pos - snippet_len : 0;

	Datum values[4];
	bool nulls[4] = {false};
	values[0] = BoolGetDatum(found);
	values[1] = Int64GetDatum(iterations * batch);
	values[2] = Int64GetDatum((int64_t)vm_shared_state->cpu.instret);
	values[3] = PointerGetDatum(cstring_to_text_with_len(vm_shared_state->console_buf + snippet_start, snippet_len));

	elog(WARNING, "DEBUG: tuple text bounds constructed cleanly");

	HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
	elog(WARNING, "DEBUG: tuple layout constructed");
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * vm_console_instret() → SETOF RECORD(line_num, instret, line_text)
 *
 * Return the instret value when each console line was printed.
 */
PG_FUNCTION_INFO_V1(rv64_vm_console_instret);
Datum
rv64_vm_console_instret(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;

	if (SRF_IS_FIRSTCALL()) {
		funcctx = SRF_FIRSTCALL_INIT();
		if (!vm_shared_state || vm_shared_state->cpu.dbg.console_log_count == 0)
			SRF_RETURN_DONE(funcctx);

		MemoryContext old = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		int total = vm_shared_state->cpu.dbg.console_log_count;
		if (total > DBG_CONSOLE_LOG_SIZE) total = DBG_CONSOLE_LOG_SIZE;
		funcctx->max_calls = total;

		TupleDesc tupdesc = CreateTemplateTupleDesc(3);
		TupleDescInitEntry(tupdesc, 1, "line_num", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, 2, "instret", INT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, 3, "line_text", TEXTOID, -1, 0);
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		MemoryContextSwitchTo(old);
	}

	funcctx = SRF_PERCALL_SETUP();

	if (funcctx->call_cntr < funcctx->max_calls) {
		int total = vm_shared_state->cpu.dbg.console_log_count;
		int start = (total > DBG_CONSOLE_LOG_SIZE)
			? total % DBG_CONSOLE_LOG_SIZE : 0;
		int idx = (start + funcctx->call_cntr) % DBG_CONSOLE_LOG_SIZE;

		int pos = vm_shared_state->cpu.dbg.console_log[idx].console_pos;
		int line_start = pos - 1;
		while (line_start > 0 && vm_shared_state->console_buf[line_start - 1] != '\n')
			line_start--;
		int line_len = pos - line_start - 1;
		if (line_len < 0) line_len = 0;
		if (line_len > 500) line_len = 500;

		char line_buf[512];
		if (line_len > 0)
			memcpy(line_buf, vm_shared_state->console_buf + line_start, line_len);
		line_buf[line_len] = '\0';

		Datum vals[3];
		bool nulls[3] = {false};
		vals[0] = Int32GetDatum((int32_t)funcctx->call_cntr);
		vals[1] = Int64GetDatum((int64_t)vm_shared_state->cpu.dbg.console_log[idx].instret);
		vals[2] = CStringGetTextDatum(line_buf);

		HeapTuple tuple = heap_form_tuple(funcctx->tuple_desc, vals, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}

/* ================================================================
 * Phase 0 Observability — full internal state visible from SQL.
 * ================================================================ */

PG_FUNCTION_INFO_V1(rv64_vm_dbg_state);
Datum
rv64_vm_dbg_state(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_dbg_state: no VM initialized");

	TupleDesc tupdesc;
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "vm_dbg_state: must be called as composite-returning function");
	tupdesc = BlessTupleDesc(tupdesc);

	Datum values[20];
	bool nulls[20] = {false};

	values[0]  = Int64GetDatum((int64_t)vm_shared_state->cpu.cycle);
	values[1]  = Int64GetDatum((int64_t)vm_shared_state->cpu.instret);
	values[2]  = BoolGetDatum(vm_shared_state->cpu.halted != 0);
	values[3]  = BoolGetDatum(vm_shared_state->cpu.error != 0);
	values[4]  = Int32GetDatum((int32_t)vm_shared_state->cpu.run_exit_reason);
	values[5]  = BoolGetDatum(vm_shared_state->cpu.dbg.debug_halt != 0);
	values[6]  = BoolGetDatum(vm_shared_state->cpu.dbg.break_active != 0);
	values[7]  = Int64GetDatum((int64_t)vm_shared_state->cpu.dbg.break_pc);
	values[8]  = BoolGetDatum(vm_shared_state->cpu.dbg.break_halt != 0);
	values[9]  = Int32GetDatum(vm_shared_state->cpu.dbg.break_log_count);
	values[10] = BoolGetDatum(vm_shared_state->cpu.dbg.watch_active != 0);
	values[11] = Int64GetDatum((int64_t)vm_shared_state->cpu.dbg.watch_addr);
	values[12] = Int32GetDatum(vm_shared_state->cpu.dbg.store_log_count);
	values[13] = Int32GetDatum(vm_shared_state->cpu.dbg.console_log_count);
	/* Trap and FPU state */
	values[14] = Int64GetDatum((int64_t)vm_shared_state->cpu.satp);
	values[15] = Int32GetDatum((int32_t)vm_shared_state->cpu.fcsr);
	values[16] = Int64GetDatum((int64_t)vm_shared_state->cpu.mepc);
	values[17] = Int64GetDatum((int64_t)vm_shared_state->cpu.mcause);
	values[18] = Int64GetDatum((int64_t)vm_shared_state->cpu.sepc);
	values[19] = Int64GetDatum((int64_t)vm_shared_state->cpu.scause);

	HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * vm_csrs() → RECORD
 *
 * All trap/privilege CSRs in one row for comprehensive introspection.
 */
PG_FUNCTION_INFO_V1(rv64_vm_csrs);
Datum
rv64_vm_csrs(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_csrs: no VM initialized");

	TupleDesc tupdesc;
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "vm_csrs: must be called as composite-returning function");
	tupdesc = BlessTupleDesc(tupdesc);

	Datum values[15];
	bool nulls[15] = {false};

	values[0]  = Int64GetDatum((int64_t)vm_shared_state->cpu.mstatus);
	values[1]  = Int64GetDatum((int64_t)vm_shared_state->cpu.medeleg);
	values[2]  = Int64GetDatum((int64_t)vm_shared_state->cpu.mideleg);
	values[3]  = Int64GetDatum((int64_t)vm_shared_state->cpu.mie);
	values[4]  = Int64GetDatum((int64_t)vm_shared_state->cpu.mip);
	values[5]  = Int64GetDatum((int64_t)vm_shared_state->cpu.mtvec);
	values[6]  = Int64GetDatum((int64_t)vm_shared_state->cpu.mepc);
	values[7]  = Int64GetDatum((int64_t)vm_shared_state->cpu.mcause);
	values[8]  = Int64GetDatum((int64_t)vm_shared_state->cpu.mtval);
	values[9]  = Int64GetDatum((int64_t)vm_shared_state->cpu.stvec);
	values[10] = Int64GetDatum((int64_t)vm_shared_state->cpu.sepc);
	values[11] = Int64GetDatum((int64_t)vm_shared_state->cpu.scause);
	values[12] = Int64GetDatum((int64_t)vm_shared_state->cpu.stval);
	values[13] = Int64GetDatum((int64_t)vm_shared_state->cpu.satp);
	values[14] = Int32GetDatum((int32_t)vm_shared_state->cpu.priv);

	HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

PG_FUNCTION_INFO_V1(rv64_vm_timer_state);
Datum
rv64_vm_timer_state(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_timer_state: no VM initialized");

	TupleDesc tupdesc;
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "vm_timer_state: must be called as composite-returning function");
	tupdesc = BlessTupleDesc(tupdesc);

	int64_t mtime = (int64_t)vm_shared_state->cpu.clint.mtime;
	int64_t mtimecmp = (int64_t)vm_shared_state->cpu.clint.mtimecmp;
	int64_t stimecmp = (int64_t)vm_shared_state->cpu.stimecmp;
	int64_t delta_m = mtimecmp - mtime;
	int64_t delta_s = stimecmp - mtime;

	Datum values[5];
	bool nulls[5] = {false};
	values[0] = Int64GetDatum(mtime);
	values[1] = Int64GetDatum(mtimecmp);
	values[2] = Int64GetDatum(stimecmp);
	values[3] = Int64GetDatum(delta_m);
	values[4] = Int64GetDatum(delta_s);

	HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

PG_FUNCTION_INFO_V1(rv64_vm_run_reason);
Datum
rv64_vm_run_reason(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_run_reason: no VM initialized");

	const char *reason;
	switch (vm_shared_state->cpu.run_exit_reason) {
	case RUN_EXIT_NONE:        reason = "none (not run yet)"; break;
	case RUN_EXIT_BUDGET:      reason = "budget exhausted"; break;
	case RUN_EXIT_ERROR:       reason = "fatal error (rv64_step returned -1)"; break;
	case RUN_EXIT_DEBUG_HALT:  reason = "debug_halt (breakpoint or vm_step_until)"; break;
	case RUN_EXIT_WFI_TIMEOUT: reason = "wfi_timeout (halted, budget exhausted)"; break;
	default:                   reason = "unknown"; break;
	}

	PG_RETURN_TEXT_P(cstring_to_text(reason));
}

/* ================================================================
 * VM Snapshots — save/restore/query VM state to/from files.
 * ================================================================ */

#define SNAPSHOT_MAGIC   0x485350414E535652ULL  /* "RVSNAPSH" */
#define SNAPSHOT_VERSION 1

struct vm_snapshot_header {
	uint64_t magic;
	uint32_t version;
	uint64_t struct_size;    /* sizeof(struct rv64_cpu) */
	uint64_t ram_size;       /* cpu->ram_size */
	uint32_t console_size;   /* vm_shared_state->console_pos at time of save */
	/* Quick-access fields (readable without loading full snapshot) */
	uint64_t pc;
	uint64_t instret;
	uint64_t cycle;
	uint8_t  priv;
	uint64_t mtime;
	uint64_t satp;
	int32_t  halted;
	int32_t  error;
	int32_t  run_exit_reason;
	uint64_t mstatus;
	uint32_t fcsr;
};

/*
 * vm_save(path TEXT) → TEXT
 *
 * Save VM state + RAM + console to a file.
 * Format: header | cpu struct | ram | console
 */
PG_FUNCTION_INFO_V1(rv64_vm_save);
Datum
rv64_vm_save(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_save: no VM initialized");

	char *path = text_to_cstring(PG_GETARG_TEXT_PP(0));

	FILE *f = fopen(path, "wb");
	if (!f)
		elog(ERROR, "vm_save: cannot open %s for writing", path);

	/* Build header */
	struct vm_snapshot_header hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = SNAPSHOT_MAGIC;
	hdr.version = SNAPSHOT_VERSION;
	hdr.struct_size = sizeof(struct rv64_cpu);
	hdr.ram_size = vm_shared_state->cpu.ram_size;
	hdr.console_size = vm_shared_state->console_pos;
	hdr.pc = vm_shared_state->cpu.pc;
	hdr.instret = vm_shared_state->cpu.instret;
	hdr.cycle = vm_shared_state->cpu.cycle;
	hdr.priv = vm_shared_state->cpu.priv;
	hdr.mtime = vm_shared_state->cpu.clint.mtime;
	hdr.satp = vm_shared_state->cpu.satp;
	hdr.halted = vm_shared_state->cpu.halted;
	hdr.error = vm_shared_state->cpu.error;
	hdr.run_exit_reason = (int32_t)vm_shared_state->cpu.run_exit_reason;
	hdr.mstatus = vm_shared_state->cpu.mstatus;
	hdr.fcsr = vm_shared_state->cpu.fcsr;

	/* Ensure frontend has RAM mapped for writing */
	ensure_frontend_ram();

	/* Write: header, cpu struct, ram, console */
	if (fwrite(&hdr, sizeof(hdr), 1, f) != 1 ||
	    fwrite(&vm_shared_state->cpu, sizeof(struct rv64_cpu), 1, f) != 1 ||
	    fwrite(frontend_ram, 1, vm_shared_state->cpu.ram_size, f) != vm_shared_state->cpu.ram_size ||
	    fwrite(vm_shared_state->console_buf, 1, vm_shared_state->console_pos, f) != (size_t)vm_shared_state->console_pos) {
		fclose(f);
		elog(ERROR, "vm_save: write error");
	}

	fclose(f);

	char buf[256];
	snprintf(buf, sizeof(buf), "saved: %s (%.1f MB, instret=%lu)",
	         path, (sizeof(hdr) + sizeof(struct rv64_cpu) + vm_shared_state->cpu.ram_size + vm_shared_state->console_pos) / 1048576.0,
	         (unsigned long)vm_shared_state->cpu.instret);
	pfree(path);
	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

/*
 * vm_load(path TEXT) → TEXT
 *
 * Restore VM state from a snapshot file.
 * Fixes up internal pointers (ram, TLB, UART, JIT).
 */
PG_FUNCTION_INFO_V1(rv64_vm_load);
Datum
rv64_vm_load(PG_FUNCTION_ARGS)
{
	char *path = text_to_cstring(PG_GETARG_TEXT_PP(0));

	FILE *f = fopen(path, "rb");
	if (!f)
		elog(ERROR, "vm_load: cannot open %s", path);

	/* Read and verify header */
	struct vm_snapshot_header hdr;
	if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
		fclose(f);
		elog(ERROR, "vm_load: cannot read header");
	}
	if (hdr.magic != SNAPSHOT_MAGIC) {
		fclose(f);
		elog(ERROR, "vm_load: bad magic (not a VM snapshot)");
	}
	if (hdr.version != SNAPSHOT_VERSION) {
		fclose(f);
		elog(ERROR, "vm_load: version mismatch (%u vs %u)", hdr.version, SNAPSHOT_VERSION);
	}
	if (hdr.struct_size != sizeof(struct rv64_cpu)) {
		fclose(f);
		elog(ERROR, "vm_load: struct size mismatch (%lu vs %lu) — rebuild needed",
		     (unsigned long)hdr.struct_size, (unsigned long)sizeof(struct rv64_cpu));
	}

	/* Allocate (or reuse) buffers */
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		vm_cpu = palloc0(sizeof(struct rv64_cpu));
	if (!vm_ram || vm_cpu->ram_size != hdr.ram_size) {
		if (vm_ram) pfree(vm_ram);
		vm_ram = palloc0(hdr.ram_size);
	}
	vm_shared_state->console_pos = 0;

	/* Read cpu struct */
	if (fread(vm_cpu, sizeof(struct rv64_cpu), 1, f) != 1) {
		fclose(f);
		elog(ERROR, "vm_load: cannot read cpu struct");
	}

	/* Read RAM */
	if (fread(vm_ram, 1, hdr.ram_size, f) != hdr.ram_size) {
		fclose(f);
		elog(ERROR, "vm_load: cannot read RAM");
	}

	/* Read console */
	vm_shared_state->console_pos = hdr.console_size;
	if (vm_shared_state->console_pos > CONSOLE_BUF_SIZE - 1)
		vm_shared_state->console_pos = CONSOLE_BUF_SIZE - 1;
	if (vm_shared_state->console_pos > 0) {
		if (fread(vm_shared_state->console_buf, 1, vm_shared_state->console_pos, f) != (size_t)vm_shared_state->console_pos) {
			fclose(f);
			elog(ERROR, "vm_load: cannot read console");
		}
	}
	vm_shared_state->console_buf[vm_shared_state->console_pos] = '\0';

	fclose(f);

	/* Fix up internal pointers */
	/* Removed explicit vm_cpu->ram assignment to respect process locality */
#ifndef DISABLE_JIT
	vm_cpu->jit = calloc(1, sizeof(struct jit_state));
	if (vm_cpu->jit && jit_init(vm_cpu->jit) < 0) {
		free(vm_cpu->jit);
		vm_cpu->jit = NULL;
	}
#else
	vm_cpu->jit = NULL;
#endif
	vm_cpu->uart.tx_callback = pg_uart_tx;
	vm_cpu->uart.tx_opaque = NULL;

	/* Flush TLB — host_base pointers are stale */
	memset(vm_cpu->tlb, 0, sizeof(vm_cpu->tlb));

	/* Clear debug halt so the VM is ready to run */
	vm_cpu->dbg.debug_halt = 0;

	char buf[256];
	snprintf(buf, sizeof(buf), "loaded: %s (instret=%lu, pc=0x%lx, priv=%d)",
	         path, (unsigned long)vm_cpu->instret,
	         (unsigned long)vm_cpu->pc, vm_cpu->priv);
	pfree(path);
	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

/*
 * vm_snapshot_info(path TEXT) → RECORD
 *
 * Read ONLY the header from a snapshot file — no loading.
 * Returns key state for quick inspection.
 */
PG_FUNCTION_INFO_V1(rv64_vm_snapshot_info);
Datum
rv64_vm_snapshot_info(PG_FUNCTION_ARGS)
{
	char *path = text_to_cstring(PG_GETARG_TEXT_PP(0));

	FILE *f = fopen(path, "rb");
	if (!f)
		elog(ERROR, "vm_snapshot_info: cannot open %s", path);

	struct vm_snapshot_header hdr;
	if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
		fclose(f);
		elog(ERROR, "vm_snapshot_info: cannot read header");
	}
	fclose(f);

	if (hdr.magic != SNAPSHOT_MAGIC)
		elog(ERROR, "vm_snapshot_info: not a VM snapshot");

	TupleDesc tupdesc;
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "vm_snapshot_info: must be called as composite-returning function");
	tupdesc = BlessTupleDesc(tupdesc);

	Datum values[12];
	bool nulls[12] = {false};
	values[0]  = Int64GetDatum((int64_t)hdr.pc);
	values[1]  = Int32GetDatum((int32_t)hdr.priv);
	values[2]  = Int64GetDatum((int64_t)hdr.instret);
	values[3]  = Int64GetDatum((int64_t)hdr.cycle);
	values[4]  = Int64GetDatum((int64_t)hdr.mtime);
	values[5]  = BoolGetDatum(hdr.halted != 0);
	values[6]  = BoolGetDatum(hdr.error != 0);
	values[7]  = Int32GetDatum(hdr.run_exit_reason);
	values[8]  = Int64GetDatum((int64_t)hdr.satp);
	values[9]  = Int64GetDatum((int64_t)hdr.mstatus);
	values[10] = Int32GetDatum((int32_t)hdr.ram_size / (1024*1024)); /* MB */
	values[11] = Int32GetDatum((int32_t)hdr.console_size);

	pfree(path);

	HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/* ================================================================
 * vm_jit_set — enable or disable the JIT compiler from SQL
 * ================================================================ */

PG_FUNCTION_INFO_V1(rv64_vm_jit_set);
Datum
rv64_vm_jit_set(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
		                errmsg("vm_jit_set: no VM initialized")));

	bool enable = PG_GETARG_BOOL(0);

	if (!vm_shared_state->cpu.jit) {
		if (enable)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			                errmsg("vm_jit_set: JIT compiler not supported on this host (mmap failed or wrong arch)")));
		PG_RETURN_TEXT_P(cstring_to_text("JIT compiler is permanently unavailable."));
	}

	/* TODO: implement VM_CMD_JIT_SET IPC command to toggle jit->enabled in worker */
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
	                errmsg("vm_jit_set: toggling JIT requires IPC command (not yet implemented)")));
}

/* ================================================================
 * vm_reset — delete saved VM state, force cold boot on next vm_boot.
 * ================================================================ */

PG_FUNCTION_INFO_V1(rv64_vm_reset);
Datum
rv64_vm_reset(PG_FUNCTION_ARGS)
{
	PushActiveSnapshot(GetTransactionSnapshot());
	SPI_connect();

	/* Read and unlink any existing large objects */
	int ret = SPI_execute("SELECT ram_oid, disk_oid FROM vm_state WHERE id = 1", true, 1);
	if (ret == SPI_OK_SELECT && SPI_processed == 1) {
		bool isnull;
		Datum d;

		d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
		if (!isnull) {
			Oid ram_oid = DatumGetObjectId(d);
			if (OidIsValid(ram_oid))
				inv_drop(ram_oid);
		}
		d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull);
		if (!isnull) {
			Oid disk_oid = DatumGetObjectId(d);
			if (OidIsValid(disk_oid))
				inv_drop(disk_oid);
		}
	}

	SPI_execute("DELETE FROM vm_state WHERE id = 1", false, 0);

	SPI_finish();
	PopActiveSnapshot();

	/* Also destroy in-process VM so next vm_boot cold-boots */
	if (vm_cpu)  { pfree(vm_cpu);  vm_cpu = NULL; }
	if (vm_ram_dsm) {
		dsm_unpin_mapping(vm_ram_dsm);
		dsm_detach(vm_ram_dsm);
		vm_ram_dsm = NULL;
		vm_ram = NULL;
	}
	if (vm_disk) { free(vm_disk); vm_disk = NULL; }
	if (vm_disk_dirty) { free(vm_disk_dirty); vm_disk_dirty = NULL; }
	vm_shared_state->console_pos = 0;
	vm_shared_state->console_pos = 0;
	vm_disk_size = 0;

	PG_RETURN_TEXT_P(cstring_to_text("VM state cleared — next vm_boot() will cold boot"));
}

/* ================================================================
 * vm_halt — destroy in-process VM and gracefully release DSM without deleting state.
 * ================================================================ */

PG_FUNCTION_INFO_V1(rv64_vm_halt);
Datum
rv64_vm_halt(PG_FUNCTION_ARGS)
{
	/* Destroy in-process VM components if present natively */
	if (vm_dsm != NULL && vm_shared_state != NULL && vm_shared_state->worker_ready) {
		ipc_send_and_wait(VM_CMD_HALT, 0);
	}
	
	if (vm_cpu)  { pfree(vm_cpu);  vm_cpu = NULL; }
	if (vm_ram_dsm) {
		dsm_unpin_mapping(vm_ram_dsm);
		dsm_detach(vm_ram_dsm);
		vm_ram_dsm = NULL;
		vm_ram = NULL;
	}
	if (vm_disk) { free(vm_disk); vm_disk = NULL; }
	if (vm_disk_dirty) { free(vm_disk_dirty); vm_disk_dirty = NULL; }
	vm_shared_state->console_pos = 0;
	vm_shared_state->console_pos = 0;
	vm_disk_size = 0;
	
	if (vm_dsm) { dsm_detach(vm_dsm); vm_dsm = NULL; vm_shared_state = NULL; }

	PG_RETURN_TEXT_P(cstring_to_text("VM execution halted and memory cleanly unmapped."));
}

/* ================================================================
 * Introspection: FP registers, memory write, disassembly.
 * ================================================================ */

static const char *fp_abi_names[32] = {
	"ft0","ft1","ft2","ft3","ft4","ft5","ft6","ft7",
	"fs0","fs1","fa0","fa1","fa2","fa3","fa4","fa5",
	"fa6","fa7","fs2","fs3","fs4","fs5","fs6","fs7",
	"fs8","fs9","fs10","fs11","ft8","ft9","ft10","ft11"
};

/*
 * vm_fregs() → SETOF RECORD(reg, name, hex, as_float, as_double)
 */
PG_FUNCTION_INFO_V1(rv64_vm_fregs);
Datum
rv64_vm_fregs(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;

	if (SRF_IS_FIRSTCALL()) {
		funcctx = SRF_FIRSTCALL_INIT();
		ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
			elog(ERROR, "vm_fregs: no VM initialized");

		MemoryContext old = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		funcctx->max_calls = 32;
		TupleDesc tupdesc = CreateTemplateTupleDesc(5);
		TupleDescInitEntry(tupdesc, 1, "reg",       INT4OID,    -1, 0);
		TupleDescInitEntry(tupdesc, 2, "name",      TEXTOID,    -1, 0);
		TupleDescInitEntry(tupdesc, 3, "hex",       TEXTOID,    -1, 0);
		TupleDescInitEntry(tupdesc, 4, "as_float",  FLOAT4OID,  -1, 0);
		TupleDescInitEntry(tupdesc, 5, "as_double", FLOAT8OID,  -1, 0);
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		MemoryContextSwitchTo(old);
	}

	funcctx = SRF_PERCALL_SETUP();
	if (funcctx->call_cntr < funcctx->max_calls) {
		int i = funcctx->call_cntr;
		uint64_t raw = vm_shared_state->cpu.f[i];

		/* Decode float: if upper 32 bits are all 1s, it's NaN-boxed single */
		float fval;
		uint32_t lo = (uint32_t)(raw & 0xFFFFFFFF);
		memcpy(&fval, &lo, 4);

		double dval;
		memcpy(&dval, &raw, 8);

		char hex[20];
		snprintf(hex, sizeof(hex), "0x%016lx", (unsigned long)raw);

		Datum vals[5];
		bool nulls[5] = {false};
		vals[0] = Int32GetDatum(i);
		vals[1] = CStringGetTextDatum(fp_abi_names[i]);
		vals[2] = CStringGetTextDatum(hex);
		vals[3] = Float4GetDatum(fval);
		vals[4] = Float8GetDatum(dval);

		HeapTuple tuple = heap_form_tuple(funcctx->tuple_desc, vals, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	SRF_RETURN_DONE(funcctx);
}

/* ---- Disassembler ---- */

static const char *int_abi[32] = {
	"zero","ra","sp","gp","tp","t0","t1","t2",
	"s0","s1","a0","a1","a2","a3","a4","a5",
	"a6","a7","s2","s3","s4","s5","s6","s7",
	"s8","s9","s10","s11","t3","t4","t5","t6"
};

static int64_t sext(uint32_t val, int bits)
{
	uint32_t mask = 1U << (bits - 1);
	return (int64_t)((int32_t)((val ^ mask) - mask));
}

static void disasm_one(uint32_t insn, uint64_t pc, char *buf, int buflen)
{
	uint32_t opcode = insn & 0x7F;
	uint32_t rd  = (insn >> 7) & 0x1F;
	uint32_t f3  = (insn >> 12) & 0x7;
	uint32_t rs1 = (insn >> 15) & 0x1F;
	uint32_t rs2 = (insn >> 20) & 0x1F;
	uint32_t f7  = (insn >> 25) & 0x7F;

	switch (opcode) {
	case 0x37: /* LUI */
		snprintf(buf, buflen, "lui     %s, 0x%x", int_abi[rd], insn >> 12);
		return;
	case 0x17: /* AUIPC */
		snprintf(buf, buflen, "auipc   %s, 0x%x", int_abi[rd], insn >> 12);
		return;
	case 0x6F: { /* JAL */
		int32_t off = (int32_t)((((insn >> 31) & 1) << 20) |
		              (((insn >> 21) & 0x3FF) << 1) |
		              (((insn >> 20) & 1) << 11) |
		              (((insn >> 12) & 0xFF) << 12));
		off = (off << 11) >> 11; /* sign extend 21-bit */
		snprintf(buf, buflen, "jal     %s, 0x%lx", int_abi[rd], (unsigned long)(pc + off));
		return;
	}
	case 0x67: /* JALR */
		snprintf(buf, buflen, "jalr    %s, %ld(%s)", int_abi[rd],
		         (long)sext(insn >> 20, 12), int_abi[rs1]);
		return;
	case 0x63: { /* Branch */
		const char *ops[] = {"beq","bne","?","?","blt","bge","bltu","bgeu"};
		int32_t off = (int32_t)((((insn >> 31) & 1) << 12) |
		              (((insn >> 7) & 1) << 11) |
		              (((insn >> 25) & 0x3F) << 5) |
		              (((insn >> 8) & 0xF) << 1));
		off = (off << 19) >> 19;
		snprintf(buf, buflen, "%-7s %s, %s, 0x%lx", ops[f3],
		         int_abi[rs1], int_abi[rs2], (unsigned long)(pc + off));
		return;
	}
	case 0x03: { /* Loads */
		const char *ops[] = {"lb","lh","lw","ld","lbu","lhu","lwu","?"};
		snprintf(buf, buflen, "%-7s %s, %ld(%s)", ops[f3],
		         int_abi[rd], (long)sext(insn >> 20, 12), int_abi[rs1]);
		return;
	}
	case 0x23: { /* Stores */
		const char *ops[] = {"sb","sh","sw","sd","?","?","?","?"};
		int32_t imm = ((insn >> 25) << 5) | ((insn >> 7) & 0x1F);
		snprintf(buf, buflen, "%-7s %s, %ld(%s)", ops[f3],
		         int_abi[rs2], (long)sext(imm, 12), int_abi[rs1]);
		return;
	}
	case 0x13: { /* I-type ALU */
		const char *ops[] = {"addi","slli","slti","sltiu","xori","srli","ori","andi"};
		int64_t imm = sext(insn >> 20, 12);
		if (f3 == 1 || f3 == 5) imm = rs2; /* shift amount */
		snprintf(buf, buflen, "%-7s %s, %s, %ld", ops[f3],
		         int_abi[rd], int_abi[rs1], (long)imm);
		return;
	}
	case 0x1B: { /* I-type W ALU */
		const char *ops[] = {"addiw","slliw","?","?","?","srliw","?","?"};
		snprintf(buf, buflen, "%-7s %s, %s, %ld", ops[f3],
		         int_abi[rd], int_abi[rs1], (long)sext(insn >> 20, 12));
		return;
	}
	case 0x33: { /* R-type ALU */
		if (f7 == 1) { /* M extension */
			const char *mops[] = {"mul","mulh","mulhsu","mulhu","div","divu","rem","remu"};
			snprintf(buf, buflen, "%-7s %s, %s, %s", mops[f3],
			         int_abi[rd], int_abi[rs1], int_abi[rs2]);
			return;
		}
		const char *ops_0[] = {"add","sll","slt","sltu","xor","srl","or","and"};
		const char *ops_20[] = {"sub","?","?","?","?","sra","?","?"};
		snprintf(buf, buflen, "%-7s %s, %s, %s",
		         f7 == 0x20 ? ops_20[f3] : ops_0[f3],
		         int_abi[rd], int_abi[rs1], int_abi[rs2]);
		return;
	}
	case 0x3B: { /* R-type W ALU */
		if (f7 == 1) {
			const char *mops[] = {"mulw","?","?","?","divw","divuw","remw","remuw"};
			snprintf(buf, buflen, "%-7s %s, %s, %s", mops[f3],
			         int_abi[rd], int_abi[rs1], int_abi[rs2]);
			return;
		}
		snprintf(buf, buflen, "%-7s %s, %s, %s",
		         f7 == 0x20 ? (f3 == 0 ? "subw" : "sraw") : (f3 == 0 ? "addw" : "sllw"),
		         int_abi[rd], int_abi[rs1], int_abi[rs2]);
		return;
	}
	/* FP loads/stores */
	case 0x07: /* FLW/FLD */
		snprintf(buf, buflen, "%-7s f%d, %ld(%s)", f3 == 2 ? "flw" : "fld",
		         rd, (long)sext(insn >> 20, 12), int_abi[rs1]);
		return;
	case 0x27: { /* FSW/FSD */
		int32_t imm = ((insn >> 25) << 5) | ((insn >> 7) & 0x1F);
		snprintf(buf, buflen, "%-7s f%d, %ld(%s)", f3 == 2 ? "fsw" : "fsd",
		         rs2, (long)sext(imm, 12), int_abi[rs1]);
		return;
	}
	/* FP arithmetic */
	case 0x53: {
		uint32_t fmt = (f7 >> 2);
		const char *sfx = (f7 & 1) ? ".d" : ".s";
		switch (fmt) {
		case 0: snprintf(buf, buflen, "fadd%s  f%d, f%d, f%d", sfx, rd, rs1, rs2); return;
		case 1: snprintf(buf, buflen, "fsub%s  f%d, f%d, f%d", sfx, rd, rs1, rs2); return;
		case 2: snprintf(buf, buflen, "fmul%s  f%d, f%d, f%d", sfx, rd, rs1, rs2); return;
		case 3: snprintf(buf, buflen, "fdiv%s  f%d, f%d, f%d", sfx, rd, rs1, rs2); return;
		case 0x0B: snprintf(buf, buflen, "fsqrt%s f%d, f%d", sfx, rd, rs1); return;
		case 4:
			if (f3 == 0) snprintf(buf, buflen, "fsgnj%s f%d, f%d, f%d", sfx, rd, rs1, rs2);
			else if (f3 == 1) snprintf(buf, buflen, "fsgnjn%s f%d, f%d, f%d", sfx, rd, rs1, rs2);
			else snprintf(buf, buflen, "fsgnjx%s f%d, f%d, f%d", sfx, rd, rs1, rs2);
			return;
		case 5:
			snprintf(buf, buflen, "%s%s f%d, f%d, f%d", f3 == 0 ? "fmin" : "fmax", sfx, rd, rs1, rs2);
			return;
		case 0x14:
			if (f3 == 2) snprintf(buf, buflen, "feq%s   %s, f%d, f%d", sfx, int_abi[rd], rs1, rs2);
			else if (f3 == 1) snprintf(buf, buflen, "flt%s   %s, f%d, f%d", sfx, int_abi[rd], rs1, rs2);
			else snprintf(buf, buflen, "fle%s   %s, f%d, f%d", sfx, int_abi[rd], rs1, rs2);
			return;
		case 0x18: /* FCVT.W.S etc */
			snprintf(buf, buflen, "fcvt.w%s.%s %s, f%d", rs2 ? "u" : "", sfx+1, int_abi[rd], rs1);
			return;
		case 0x1A: /* FCVT.S.W etc */
			snprintf(buf, buflen, "fcvt%s.w%s f%d, %s", sfx, rs2 ? "u" : "", rd, int_abi[rs1]);
			return;
		case 0x1C:
			if (f3 == 0) snprintf(buf, buflen, "fmv.x.w %s, f%d", int_abi[rd], rs1);
			else snprintf(buf, buflen, "fclass%s %s, f%d", sfx, int_abi[rd], rs1);
			return;
		case 0x1E:
			snprintf(buf, buflen, "fmv.w.x f%d, %s", rd, int_abi[rs1]);
			return;
		case 0x08: /* FCVT.S.D / FCVT.D.S */
			if (f7 & 1) snprintf(buf, buflen, "fcvt.d.s f%d, f%d", rd, rs1);
			else snprintf(buf, buflen, "fcvt.s.d f%d, f%d", rd, rs1);
			return;
		default:
			snprintf(buf, buflen, "fp_op   f7=0x%02x f3=%d rd=%d rs1=%d rs2=%d", f7, f3, rd, rs1, rs2);
			return;
		}
	}
	/* FMA */
	case 0x43: case 0x47: case 0x4B: case 0x4F: {
		const char *names[] = {"fmadd","fmsub","fnmsub","fnmadd"};
		int idx = (opcode - 0x43) / 4;
		const char *sfx = ((insn >> 25) & 1) ? ".d" : ".s";
		uint32_t rs3 = (insn >> 27) & 0x1F;
		snprintf(buf, buflen, "%s%s f%d, f%d, f%d, f%d", names[idx], sfx, rd, rs1, rs2, rs3);
		return;
	}
	/* Atomics */
	case 0x2F: {
		uint32_t op = f7 >> 2;
		const char *anames[] = {"amoadd","amoswap","lr","sc","amoxor","?","amoor","amoand",
		                        "amomin","amomax","amominu","amomaxu"};
		const char *n = (op < 12) ? anames[op] : "amo?";
		const char *w = f3 == 2 ? ".w" : ".d";
		if (op == 2) snprintf(buf, buflen, "lr%s     %s, (%s)", w, int_abi[rd], int_abi[rs1]);
		else if (op == 3) snprintf(buf, buflen, "sc%s     %s, %s, (%s)", w, int_abi[rd], int_abi[rs2], int_abi[rs1]);
		else snprintf(buf, buflen, "%s%s %s, %s, (%s)", n, w, int_abi[rd], int_abi[rs2], int_abi[rs1]);
		return;
	}
	/* SYSTEM */
	case 0x73: {
		if (insn == 0x00000073) { snprintf(buf, buflen, "ecall"); return; }
		if (insn == 0x00100073) { snprintf(buf, buflen, "ebreak"); return; }
		if (insn == 0x30200073) { snprintf(buf, buflen, "mret"); return; }
		if (insn == 0x10200073) { snprintf(buf, buflen, "sret"); return; }
		if (insn == 0x10500073) { snprintf(buf, buflen, "wfi"); return; }
		if ((insn >> 25) == 0x09) {
			snprintf(buf, buflen, "sfence.vma %s, %s", int_abi[rs1], int_abi[rs2]);
			return;
		}
		/* CSR instructions */
		uint32_t csr = insn >> 20;
		const char *csr_ops[] = {"?","csrrw","csrrs","csrrc","?","csrrwi","csrrsi","csrrci"};
		snprintf(buf, buflen, "%-7s %s, 0x%03x, %s", csr_ops[f3],
		         int_abi[rd], csr, f3 >= 4 ? "" : int_abi[rs1]);
		return;
	}
	case 0x0F: /* FENCE */
		snprintf(buf, buflen, "fence");
		return;
	default:
		snprintf(buf, buflen, "unknown opcode=0x%02x insn=0x%08x", opcode, insn);
		return;
	}
}

/*
 * vm_disasm(addr BIGINT, count INT) → SETOF RECORD(addr, hex, insn)
 *
 * Disassemble `count` instructions starting at `addr` in guest memory.
 * Handles both 32-bit and 16-bit (compressed) instructions.
 */
PG_FUNCTION_INFO_V1(rv64_vm_disasm);
Datum
rv64_vm_disasm(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	typedef struct {
		uint64_t start_addr;
		int count;
		int current;
		uint64_t next_pc;
	} disasm_ctx;

	if (SRF_IS_FIRSTCALL()) {
		funcctx = SRF_FIRSTCALL_INIT();
		ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
			elog(ERROR, "vm_disasm: no VM initialized");
		ensure_frontend_ram();

		MemoryContext old = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		disasm_ctx *ctx = palloc0(sizeof(disasm_ctx));
		ctx->start_addr = (uint64_t)PG_GETARG_INT64(0);
		ctx->count = PG_GETARG_INT32(1);
		ctx->current = 0;
		ctx->next_pc = ctx->start_addr;
		funcctx->user_fctx = ctx;

		funcctx->max_calls = ctx->count;
		TupleDesc tupdesc = CreateTemplateTupleDesc(3);
		TupleDescInitEntry(tupdesc, 1, "addr", TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, 2, "hex",  TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, 3, "insn", TEXTOID, -1, 0);
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		MemoryContextSwitchTo(old);
	}

	funcctx = SRF_PERCALL_SETUP();
	disasm_ctx *ctx = funcctx->user_fctx;

	if (ctx->current < ctx->count) {
		uint64_t pc = ctx->next_pc;

		uint64_t lo = 0, hi = 0;
		char addr_buf[20], hex_buf[12], insn_buf[80];
		
		snprintf(addr_buf, sizeof(addr_buf), "0x%lx", (unsigned long)pc);
		
		if (rv64_cpu_load(&vm_shared_state->cpu, pc, 2, &lo, ACCESS_EXEC) != 0) {
            snprintf(hex_buf, sizeof(hex_buf), "----");
            snprintf(insn_buf, sizeof(insn_buf), "<fault>");
            ctx->next_pc = pc + 2;
		} else {
            uint16_t half = (uint16_t)lo;
            int is_compressed = (half & 3) != 3;

            if (is_compressed) {
                snprintf(hex_buf, sizeof(hex_buf), "%04x", half);
                snprintf(insn_buf, sizeof(insn_buf), "c.??? (0x%04x)", half);
                ctx->next_pc = pc + 2;
            } else {
                rv64_cpu_load(&vm_shared_state->cpu, pc + 2, 2, &hi, ACCESS_EXEC);
                uint32_t full = half | ((uint32_t)hi << 16);
                snprintf(hex_buf, sizeof(hex_buf), "%08x", full);
                disasm_one(full, pc, insn_buf, sizeof(insn_buf));
                ctx->next_pc = pc + 4;
            }
        }
		ctx->current++;

		Datum vals[3];
		bool nulls[3] = {false};
		vals[0] = CStringGetTextDatum(addr_buf);
		vals[1] = CStringGetTextDatum(hex_buf);
		vals[2] = CStringGetTextDatum(insn_buf);

		HeapTuple tuple = heap_form_tuple(funcctx->tuple_desc, vals, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	SRF_RETURN_DONE(funcctx);
}

/* ================================================================
 * Trap Exception Log — ring buffer of non-interrupt traps.
 * ================================================================ */

PG_FUNCTION_INFO_V1(rv64_vm_trap_log);
Datum
rv64_vm_trap_log(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	typedef struct {
		int start;   /* first entry index in ring */
		int count;   /* total entries to return */
		int current; /* entries returned so far */
	} trap_log_ctx;

	if (SRF_IS_FIRSTCALL()) {
		funcctx = SRF_FIRSTCALL_INIT();
		ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
			elog(ERROR, "vm_trap_log: no VM initialized");

		MemoryContext old = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		trap_log_ctx *ctx = palloc0(sizeof(trap_log_ctx));

		int total = vm_shared_state->cpu.dbg.trap_log_count;
		if (total > DBG_TRAP_LOG_SIZE)
			ctx->count = DBG_TRAP_LOG_SIZE;
		else
			ctx->count = total;
		/* Start from oldest entry */
		if (total > DBG_TRAP_LOG_SIZE)
			ctx->start = total % DBG_TRAP_LOG_SIZE;
		else
			ctx->start = 0;
		ctx->current = 0;

		funcctx->user_fctx = ctx;
		funcctx->max_calls = ctx->count;

		TupleDesc tupdesc = CreateTemplateTupleDesc(12);
		TupleDescInitEntry(tupdesc, 1,  "seq",     INT4OID,  -1, 0);
		TupleDescInitEntry(tupdesc, 2,  "cause",   TEXTOID,  -1, 0);
		TupleDescInitEntry(tupdesc, 3,  "tval",    TEXTOID,  -1, 0);
		TupleDescInitEntry(tupdesc, 4,  "pc",      TEXTOID,  -1, 0);
		TupleDescInitEntry(tupdesc, 5,  "priv",    TEXTOID,  -1, 0);
		TupleDescInitEntry(tupdesc, 6,  "instret", INT8OID,  -1, 0);
		TupleDescInitEntry(tupdesc, 7,  "a7",      INT8OID,  -1, 0);
		TupleDescInitEntry(tupdesc, 8,  "a0",      TEXTOID,  -1, 0);
		TupleDescInitEntry(tupdesc, 9,  "a1",      TEXTOID,  -1, 0);
		TupleDescInitEntry(tupdesc, 10, "a2",      TEXTOID,  -1, 0);
		TupleDescInitEntry(tupdesc, 11, "ret_a0",  TEXTOID,  -1, 0);
		TupleDescInitEntry(tupdesc, 12, "ret_pc",  TEXTOID,  -1, 0);
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		MemoryContextSwitchTo(old);
	}

	funcctx = SRF_PERCALL_SETUP();
	trap_log_ctx *ctx = funcctx->user_fctx;

	if (ctx->current < ctx->count) {
		int idx = (ctx->start + ctx->current) % DBG_TRAP_LOG_SIZE;
		typeof(vm_shared_state->cpu.dbg.trap_log[0]) *e = &vm_shared_state->cpu.dbg.trap_log[idx];

		/* Decode cause inline for readability */
		const char *cause_name;
		switch (e->cause) {
			case 0:  cause_name = "INSN_MISALIGN";    break;
			case 1:  cause_name = "INSN_ACCESS";       break;
			case 2:  cause_name = "ILLEGAL_INSN";      break;
			case 3:  cause_name = "BREAKPOINT";         break;
			case 4:  cause_name = "LOAD_MISALIGN";     break;
			case 5:  cause_name = "LOAD_ACCESS";        break;
			case 6:  cause_name = "STORE_MISALIGN";    break;
			case 7:  cause_name = "STORE_ACCESS";       break;
			case 8:  cause_name = "ECALL_U";            break;
			case 9:  cause_name = "ECALL_S";            break;
			case 11: cause_name = "ECALL_M";            break;
			case 12: cause_name = "INSN_PAGE_FAULT";   break;
			case 13: cause_name = "LOAD_PAGE_FAULT";   break;
			case 15: cause_name = "STORE_PAGE_FAULT";  break;
			default: cause_name = "UNKNOWN";            break;
		}

		const char *priv_name;
		switch (e->priv) {
			case 0:  priv_name = "U"; break;
			case 1:  priv_name = "S"; break;
			case 3:  priv_name = "M"; break;
			default: priv_name = "?"; break;
		}

		char tval_buf[20], pc_buf[20], a0_buf[20], a1_buf[20], a2_buf[20];
		char ret_a0_buf[20], ret_pc_buf[20];
		snprintf(tval_buf,   sizeof(tval_buf),   "0x%lx", (unsigned long)e->tval);
		snprintf(pc_buf,     sizeof(pc_buf),     "0x%lx", (unsigned long)e->pc);
		snprintf(a0_buf,     sizeof(a0_buf),     "0x%lx", (unsigned long)e->a0);
		snprintf(a1_buf,     sizeof(a1_buf),     "0x%lx", (unsigned long)e->a1);
		snprintf(a2_buf,     sizeof(a2_buf),     "0x%lx", (unsigned long)e->a2);
		snprintf(ret_a0_buf, sizeof(ret_a0_buf), "0x%lx", (unsigned long)e->ret_a0);
		snprintf(ret_pc_buf, sizeof(ret_pc_buf), "0x%lx", (unsigned long)e->ret_pc);

		Datum vals[12];
		bool nulls[12] = {false};
		int seq_num = vm_shared_state->cpu.dbg.trap_log_count - ctx->count + ctx->current;
		vals[0] = Int32GetDatum(seq_num);
		vals[1] = CStringGetTextDatum(cause_name);
		vals[2] = CStringGetTextDatum(tval_buf);
		vals[3] = CStringGetTextDatum(pc_buf);
		vals[4] = CStringGetTextDatum(priv_name);
		vals[5] = Int64GetDatum((int64_t)e->instret);
		vals[6] = Int64GetDatum((int64_t)e->a7);
		vals[7] = CStringGetTextDatum(a0_buf);
		vals[8] = CStringGetTextDatum(a1_buf);
		vals[9] = CStringGetTextDatum(a2_buf);
		vals[10] = CStringGetTextDatum(ret_a0_buf);
		vals[11] = CStringGetTextDatum(ret_pc_buf);

		ctx->current++;
		HeapTuple tuple = heap_form_tuple(funcctx->tuple_desc, vals, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	SRF_RETURN_DONE(funcctx);
}

/* ================================================================
 * MMU Introspection: page table walk, physical memory, TLB dump.
 * ================================================================ */

/*
 * vm_translate(vaddr BIGINT) → RECORD
 *
 * Perform a full Sv39 page table walk and return every detail:
 * each level's PTE address, PTE value, decoded permissions,
 * the final physical address, and the reason for any fault.
 */
PG_FUNCTION_INFO_V1(rv64_vm_translate);
Datum
rv64_vm_translate(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_translate: no VM initialized");
	ensure_frontend_ram();

	uint64_t vaddr = (uint64_t)PG_GETARG_INT64(0);

	TupleDesc tupdesc;
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "vm_translate: must be called as composite-returning function");
	tupdesc = BlessTupleDesc(tupdesc);

	/* Fields: mode, satp_ppn, vpn2, vpn1, vpn0, page_offset,
	 *         pte2_addr, pte2_val, pte2_flags,
	 *         pte1_addr, pte1_val, pte1_flags,
	 *         pte0_addr, pte0_val, pte0_flags,
	 *         leaf_level, phys_addr, fault_reason */
	Datum values[18];
	bool nulls[18];
	memset(nulls, true, sizeof(nulls)); /* default all null */

	/* Mode */
	uint64_t mode = vm_shared_state->cpu.satp >> 60;
	values[0] = Int32GetDatum((int32_t)mode);
	nulls[0] = false;

	if (mode == 0) {
		/* Bare mode — identity mapping */
		values[15] = Int32GetDatum(-1);  nulls[15] = false;
		values[16] = Int64GetDatum((int64_t)vaddr); nulls[16] = false;
		values[17] = CStringGetTextDatum("bare mode (identity)"); nulls[17] = false;

		HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
		PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
	}
	if (vm_shared_state->cpu.priv == 3) { /* PRIV_M */
		values[15] = Int32GetDatum(-1);  nulls[15] = false;
		values[16] = Int64GetDatum((int64_t)vaddr); nulls[16] = false;
		values[17] = CStringGetTextDatum("M-mode (no translation)"); nulls[17] = false;

		HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
		PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
	}

	uint64_t satp_ppn = vm_shared_state->cpu.satp & 0xFFFFFFFFFFFULL;
	values[1] = Int64GetDatum((int64_t)satp_ppn); nulls[1] = false;

	uint64_t vpn[3];
	vpn[2] = (vaddr >> 30) & 0x1FF;
	vpn[1] = (vaddr >> 21) & 0x1FF;
	vpn[0] = (vaddr >> 12) & 0x1FF;
	uint64_t pg_off = vaddr & 0xFFF;

	values[2] = Int32GetDatum((int32_t)vpn[2]); nulls[2] = false;
	values[3] = Int32GetDatum((int32_t)vpn[1]); nulls[3] = false;
	values[4] = Int32GetDatum((int32_t)vpn[0]); nulls[4] = false;
	values[5] = Int32GetDatum((int32_t)pg_off); nulls[5] = false;

	uint64_t pt_addr = satp_ppn << 12;
	uint64_t pte;

	for (int level = 2; level >= 0; level--) {
		uint64_t pte_addr = pt_addr + vpn[level] * 8;
		int idx_base = 6 + (2 - level) * 3;  /* level2→6, level1→9, level0→12 */

		values[idx_base] = Int64GetDatum((int64_t)pte_addr);
		nulls[idx_base] = false;

		if (bus_load(&vm_shared_state->cpu, pte_addr, 8, &pte) < 0) {
			values[17] = CStringGetTextDatum("bus fault reading PTE");
			nulls[17] = false;
			break;
		}

		values[idx_base + 1] = Int64GetDatum((int64_t)pte);
		nulls[idx_base + 1] = false;

		/* Decode flags into human-readable string */
		char flags[32];
		snprintf(flags, sizeof(flags), "%c%c%c%c%c%c%c%c",
		         (pte & PTE_V) ? 'V' : '-',
		         (pte & PTE_R) ? 'R' : '-',
		         (pte & PTE_W) ? 'W' : '-',
		         (pte & PTE_X) ? 'X' : '-',
		         (pte & PTE_U) ? 'U' : '-',
		         (pte & PTE_G) ? 'G' : '-',
		         (pte & PTE_A) ? 'A' : '-',
		         (pte & PTE_D) ? 'D' : '-');
		values[idx_base + 2] = CStringGetTextDatum(flags);
		nulls[idx_base + 2] = false;

		if (!(pte & PTE_V)) {
			char buf[64];
			snprintf(buf, sizeof(buf), "PTE invalid at level %d", level);
			values[17] = CStringGetTextDatum(buf); nulls[17] = false;
			break;
		}

		uint64_t rwx = pte & (PTE_R | PTE_W | PTE_X);
		if (rwx == PTE_W || rwx == (PTE_W | PTE_X)) {
			values[17] = CStringGetTextDatum("reserved RWX bits"); nulls[17] = false;
			break;
		}

		if (rwx != 0) {
			/* Leaf PTE found */
			values[15] = Int32GetDatum(level); nulls[15] = false;

			uint64_t ppn = pte >> 10;
			uint64_t result;
			if (level == 2) {
				result = (ppn << 12) | (vpn[1] << 21) | (vpn[0] << 12) | pg_off;
			} else if (level == 1) {
				result = ((ppn >> 9) << 21) | (vpn[0] << 12) | pg_off;
			} else {
				result = (ppn << 12) | pg_off;
			}
			values[16] = Int64GetDatum((int64_t)result); nulls[16] = false;

			/* Check permissions for store */
			char reason[128] = "OK";
			if (!(pte & PTE_W))
				snprintf(reason, sizeof(reason), "no write permission (flags=%s)", flags);
			else if (vm_shared_state->cpu.priv == 0 && !(pte & PTE_U)) /* PRIV_U */
				snprintf(reason, sizeof(reason), "user access to supervisor page");
			else if (vm_shared_state->cpu.priv == 1 && (pte & PTE_U) && !(vm_shared_state->cpu.mstatus & MSTATUS_SUM))
				snprintf(reason, sizeof(reason), "supervisor access to user page (SUM=0)");
			values[17] = CStringGetTextDatum(reason); nulls[17] = false;
			break;
		}

		/* Non-leaf: follow next level */
		pt_addr = ((pte >> 10) & 0xFFFFFFFFFFFULL) << 12;

		if (level == 0) {
			values[17] = CStringGetTextDatum("no leaf found (ran out of levels)");
			nulls[17] = false;
		}
	}

	HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * vm_physpeek(paddr BIGINT, size INT) → BIGINT
 *
 * Read directly from physical memory, bypassing the MMU.
 * Routes through bus_load for correct MMIO handling.
 */
PG_FUNCTION_INFO_V1(rv64_vm_physpeek);
Datum
rv64_vm_physpeek(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_physpeek: no VM initialized");
	ensure_frontend_ram();

	uint64_t paddr = (uint64_t)PG_GETARG_INT64(0);
	int size = PG_GETARG_INT32(1);

	if (size != 1 && size != 2 && size != 4 && size != 8)
		elog(ERROR, "vm_physpeek: size must be 1, 2, 4, or 8");

	uint64_t val;
	if (bus_load(&vm_shared_state->cpu, paddr, size, &val) < 0)
		elog(ERROR, "vm_physpeek: bus fault at 0x%lx", (unsigned long)paddr);

	PG_RETURN_INT64((int64_t)val);
}

/*
 * vm_physpeek_hex(paddr BIGINT, nbytes INT) → TEXT
 *
 * Hex dump of physical memory.
 */
PG_FUNCTION_INFO_V1(rv64_vm_physpeek_hex);
Datum
rv64_vm_physpeek_hex(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_physpeek_hex: no VM initialized");
	ensure_frontend_ram();

	uint64_t paddr = (uint64_t)PG_GETARG_INT64(0);
	int nbytes = PG_GETARG_INT32(1);
	if (nbytes <= 0 || nbytes > 512)
		elog(ERROR, "vm_physpeek_hex: nbytes must be 1..512");

	char *buf = palloc(nbytes * 3 + 1);
	int pos = 0;
	for (int i = 0; i < nbytes; i++) {
		uint64_t val;
		if (bus_load(&vm_shared_state->cpu, paddr + i, 1, &val) < 0) {
			buf[pos++] = '?'; buf[pos++] = '?';
		} else {
			pos += snprintf(buf + pos, 4, "%02x", (unsigned)(val & 0xFF));
		}
		if (i < nbytes - 1) buf[pos++] = ' ';
	}
	buf[pos] = '\0';
	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

/*
 * vm_tlb_dump() → SETOF RECORD
 *
 * Show all valid TLB entries with their host_base pointers.
 * Key for diagnosing host memory corruption: if host_base points
 * outside vm_ram, a store through that TLB entry would corrupt
 * host memory (e.g., the CPU struct itself).
 */
PG_FUNCTION_INFO_V1(rv64_vm_tlb_dump);
Datum
rv64_vm_tlb_dump(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	typedef struct {
		int next_idx;
	} tlb_ctx;

	if (SRF_IS_FIRSTCALL()) {
		funcctx = SRF_FIRSTCALL_INIT();
		ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
			elog(ERROR, "vm_tlb_dump: no VM initialized");
		ensure_frontend_ram();

		MemoryContext old = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		tlb_ctx *ctx = palloc0(sizeof(tlb_ctx));
		ctx->next_idx = 0;
		funcctx->user_fctx = ctx;
		funcctx->max_calls = TLB_SIZE;

		TupleDesc tupdesc = CreateTemplateTupleDesc(8);
		TupleDescInitEntry(tupdesc, 1, "idx",        INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, 2, "vpn",        TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, 3, "ppn",        TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, 4, "perm",       TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, 5, "level",      INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, 6, "dirty",      BOOLOID, -1, 0);
		TupleDescInitEntry(tupdesc, 7, "host_base",  TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, 8, "host_valid", BOOLOID, -1, 0);  /* in ram range? */
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		MemoryContextSwitchTo(old);
	}

	funcctx = SRF_PERCALL_SETUP();
	tlb_ctx *ctx = funcctx->user_fctx;

	/* Skip empty entries */
	while (ctx->next_idx < TLB_SIZE && !vm_shared_state->cpu.tlb[ctx->next_idx].valid)
		ctx->next_idx++;

	if (ctx->next_idx < TLB_SIZE) {
		struct tlb_entry *te = &vm_shared_state->cpu.tlb[ctx->next_idx];

		char vpn_buf[20], ppn_buf[20], perm_buf[8], host_buf[24];
		snprintf(vpn_buf, sizeof(vpn_buf), "0x%lx", (unsigned long)te->vpn);
		snprintf(ppn_buf, sizeof(ppn_buf), "0x%lx", (unsigned long)te->ppn);
		snprintf(perm_buf, sizeof(perm_buf), "%c%c%c%c",
		         (te->perm & PTE_R) ? 'R' : '-',
		         (te->perm & PTE_W) ? 'W' : '-',
		         (te->perm & PTE_X) ? 'X' : '-',
		         (te->perm & PTE_U) ? 'U' : '-');

		/* Check if host_base is within vm_ram */
		bool host_valid = false;
		if (te->host_base != NULL) {
			snprintf(host_buf, sizeof(host_buf), "%p", (void *)te->host_base);
			host_valid = (te->host_base >= frontend_ram &&
			              te->host_base < frontend_ram + vm_shared_state->cpu.ram_size);
		} else {
			snprintf(host_buf, sizeof(host_buf), "NULL (MMIO)");
		}

		Datum vals[8];
		bool nl[8] = {false};
		vals[0] = Int32GetDatum(ctx->next_idx);
		vals[1] = CStringGetTextDatum(vpn_buf);
		vals[2] = CStringGetTextDatum(ppn_buf);
		vals[3] = CStringGetTextDatum(perm_buf);
		vals[4] = Int32GetDatum((int32_t)te->level);
		vals[5] = BoolGetDatum(te->dirty != 0);
		vals[6] = CStringGetTextDatum(host_buf);
		vals[7] = BoolGetDatum(host_valid);

		ctx->next_idx++;
		HeapTuple tuple = heap_form_tuple(funcctx->tuple_desc, vals, nl);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	SRF_RETURN_DONE(funcctx);
}

/* ================================================================
 * Host Profiling via macOS `sample`
 * ================================================================ */

PG_FUNCTION_INFO_V1(rv64_vm_profile_host);
Datum rv64_vm_profile_host(PG_FUNCTION_ARGS)
{
	int pid = PG_GETARG_INT32(0);
	int seconds = PG_GETARG_INT32(1);

	char cmd[256];
	snprintf(cmd, sizeof(cmd), "sample %d %d 10 -f /tmp/pg_sample_%d.txt >/dev/null 2>&1", pid, seconds, pid);

	elog(NOTICE, "Running: %s", cmd);
	int ret = system(cmd);
	if (ret != 0) {
		elog(ERROR, "sample command failed with code %d. Ensure you have Developer Tools mapping permissions.", ret);
	}

	char filepath[256];
	snprintf(filepath, sizeof(filepath), "/tmp/pg_sample_%d.txt", pid);

	FILE *f = fopen(filepath, "r");
	if (!f) {
		elog(ERROR, "Failed to read sample output from %s", filepath);
	}

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);

	text *res = palloc(size + VARHDRSZ);
	SET_VARSIZE(res, size + VARHDRSZ);
	
	size_t read_bytes = fread(VARDATA(res), 1, size, f);
	fclose(f);

	if (read_bytes != (size_t)size) {
		elog(ERROR, "Failed to read full sample file");
	}

	PG_RETURN_TEXT_P(res);
}

/*
 * vm_profile_guest() -> SETOF RECORD(idx int, pc int8, ra int8, fp int8)
 *
 * Dumps the array of recently captured JIT profiling samples.
 */
PG_FUNCTION_INFO_V1(rv64_vm_profile_guest);
Datum
rv64_vm_profile_guest(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;

	if (SRF_IS_FIRSTCALL()) {
		funcctx = SRF_FIRSTCALL_INIT();
		if (!vm_shared_state || vm_shared_state->cpu.prof.perf_sample_idx == 0)
			SRF_RETURN_DONE(funcctx);

		MemoryContext old = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		uint32_t total = vm_shared_state->cpu.prof.perf_sample_idx;
		if (total > DBG_PERF_LOG_SIZE) total = DBG_PERF_LOG_SIZE;
		funcctx->max_calls = total;

		TupleDesc tupdesc = CreateTemplateTupleDesc(4);
		TupleDescInitEntry(tupdesc, 1, "idx", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, 2, "pc", INT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, 3, "ra", INT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, 4, "fp", INT8OID, -1, 0);
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		MemoryContextSwitchTo(old);
	}

	funcctx = SRF_PERCALL_SETUP();

	if (funcctx->call_cntr < funcctx->max_calls) {
		uint32_t total = vm_shared_state->cpu.prof.perf_sample_idx;
		uint32_t start = (total > DBG_PERF_LOG_SIZE)
			? total % DBG_PERF_LOG_SIZE : 0;
		uint32_t read_idx = (start + funcctx->call_cntr) % DBG_PERF_LOG_SIZE;

		Datum vals[4];
		bool nulls[4] = {false};

		vals[0] = Int32GetDatum(funcctx->call_cntr);
		vals[1] = Int64GetDatum(vm_shared_state->cpu.prof.perf_samples[read_idx].pc);
		vals[2] = Int64GetDatum(vm_shared_state->cpu.prof.perf_samples[read_idx].ra);
		vals[3] = Int64GetDatum(vm_shared_state->cpu.prof.perf_samples[read_idx].fp);

		HeapTuple tup = heap_form_tuple(funcctx->tuple_desc, vals, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tup));
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * vm_profile_guest_reset() -> void
 */
PG_FUNCTION_INFO_V1(rv64_vm_profile_guest_reset);
Datum
rv64_vm_profile_guest_reset(PG_FUNCTION_ARGS)
{
	if (vm_shared_state) {
		vm_shared_state->cpu.prof.perf_sample_idx = 0;
		vm_shared_state->cpu.prof.perf_sample_tick = 0;
	}
	PG_RETURN_VOID();
}

/*
 * vm_disk_flush() → TEXT
 *
 * Persists all dirty disk blocks to the vm_blocks table via SPI.
 * Safe to call at any time outside the execution loop.
 * Returns a status string with the count of blocks flushed.
 *
 * On next vm_boot(), these blocks will be overlaid over rootfs.img,
 * making the PostgreSQL-stored blocks the authoritative disk state.
 */
PG_FUNCTION_INFO_V1(rv64_vm_disk_flush);
Datum
rv64_vm_disk_flush(PG_FUNCTION_ARGS)
{
	if (!vm_disk || !vm_disk_dirty || vm_disk_size == 0)
		PG_RETURN_TEXT_P(cstring_to_text("no disk loaded"));

	uint64_t nblocks = (vm_disk_size + DISK_BLOCK_SIZE - 1) / DISK_BLOCK_SIZE;
	uint64_t flushed = 0;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "vm_disk_flush: SPI_connect failed");

	/* Ensure vm_blocks table exists */
	SPI_execute(
		"CREATE TABLE IF NOT EXISTS vm_blocks ("
		"  blk_idx BIGINT PRIMARY KEY,"
		"  data BYTEA NOT NULL"
		")",
		false, 0);

	for (uint64_t b = 0; b < nblocks; b++) {
		if (!DISK_DIRTY_GET(b))
			continue;

		uint64_t off = b * DISK_BLOCK_SIZE;
		uint64_t len = DISK_BLOCK_SIZE;
		if (off + len > vm_disk_size)
			len = vm_disk_size - off;

		/* Build BYTEA datum directly from vm_disk — zero-copy. */
		bytea *blk_data = (bytea *)palloc(VARHDRSZ + len);
		SET_VARSIZE(blk_data, VARHDRSZ + len);
		memcpy(VARDATA(blk_data), vm_disk + off, len);

		Oid argtypes[3] = {INT8OID, INT8OID, BYTEAOID};
		Datum values[3];
		char nulls[3] = {' ', ' ', ' '};
		values[0] = Int64GetDatum((int64_t)vm_shared_state->cpu.id);
		values[1] = Int64GetDatum((int64_t)b);
		values[2] = PointerGetDatum(blk_data);

		int rc = SPI_execute_with_args(
			"INSERT INTO vm_blocks (vm_id, blk_idx, data) VALUES ($1, $2, $3)"
			" ON CONFLICT (vm_id, blk_idx) DO UPDATE SET data = EXCLUDED.data",
			3, argtypes, values, nulls, false, 0);

		if (rc == SPI_OK_INSERT || rc == SPI_OK_UPDATE)
			DISK_DIRTY_CLR(b);

		pfree(blk_data);
		flushed++;
	}

	SPI_finish();

	char result[64];
	snprintf(result, sizeof(result), "flushed %lu dirty blocks to vm_blocks",
	         (unsigned long)flushed);
	PG_RETURN_TEXT_P(cstring_to_text(result));
}

/* ================================================================
 * Parallel JIT Compilation Background Worker Entry Point
 * ================================================================ */

PGDLLEXPORT void rv64_jit_parallel_worker(dsm_segment *seg, shm_toc *toc);

void
rv64_jit_parallel_worker(dsm_segment *seg, shm_toc *toc)
{
	/* 1. Retrieve the RAM handle */
	dsm_handle *ram_handle = shm_toc_lookup(toc, 0, false);
	dsm_segment *ram_seg = dsm_attach(*ram_handle);
	if (!ram_seg) elog(ERROR, "JIT Worker: Unable to attach to vm_ram DSM");
	uint8_t *ram = dsm_segment_address(ram_seg);

	/* 2. Retrieve Host Function Pointers to bypass ASLR */
	uint64_t *host_ptrs = shm_toc_lookup(toc, 1, false);

	/* 3. Attach our Message Queues */
	shm_mq *mq_req = shm_toc_lookup(toc, 2, false);
	shm_mq *mq_res = shm_toc_lookup(toc, 3, false);

	shm_mq_set_receiver(mq_req, MyProc);
	shm_mq_set_sender(mq_res, MyProc);

	shm_mq_handle *mqh_req = shm_mq_attach(mq_req, seg, NULL);
	shm_mq_handle *mqh_res = shm_mq_attach(mq_res, seg, NULL);

	/* Prepare a minimal dummy CPU object so the compiler can read RAM securely */
	struct rv64_cpu dummy_cpu;
	memset(&dummy_cpu, 0, sizeof(dummy_cpu));
	vm_ram = ram;
	dummy_cpu.ram_size = 2048ULL * 1024 * 1024;
	
	/* Prepare a dedicated Background Worker JIT state */
	struct jit_state dummy_jit;
	memset(&dummy_jit, 0, sizeof(dummy_jit));
	dummy_jit.offline_mode = 1;
	dummy_jit.code_capacity = 1024 * 1024;
	dummy_jit.code_buf = palloc0(dummy_jit.code_capacity * 4);
	dummy_jit.fn_load = (void *)host_ptrs[0];
	dummy_jit.fn_store = (void *)host_ptrs[1];

	/* Reusable Payload Buffer for out-band communication */
	uint32_t payload[2048];

	/* Primary Worker Loop! */
	while (1) {
		Size nbytes;
		void *data;
		shm_mq_result req_res = shm_mq_receive(mqh_req, &nbytes, &data, false);
		if (req_res != SHM_MQ_SUCCESS)
			break; /* Main backend exited or detached! */

		if (nbytes != sizeof(uint64_t))
			continue;
			
		uint64_t target_pc = *(uint64_t *)data;

		/* Step 1: Compile locally onto dummy buffer! */
		struct jit_block b;
		uint32_t words_emitted = jit_compile_offline(&dummy_cpu, &dummy_jit, target_pc, &b);
		
		if (words_emitted == 0) {
			/* Send failure so Main thread clears pending lock */
			*(uint64_t *)(&payload[0]) = target_pc;
			payload[2] = 0;
			payload[3] = 0;
			payload[4] = 0;
			shm_mq_send(mqh_res, 5 * 4, payload, false, false);
		} else if (words_emitted > 0 && words_emitted < 1000) {
			/* Step 2: Build Payload [PC (64), insn_count(32), has_store(32), size_words(32), code...] */
			*(uint64_t *)(&payload[0]) = target_pc;
			payload[2] = b.insn_count;
			payload[3] = b.has_store;
			payload[4] = words_emitted;
			memcpy(&payload[5], dummy_jit.code_buf, words_emitted * 4);
			
			/* Step 3: Stream result to Main Backend */
			shm_mq_send(mqh_res, (5 + words_emitted) * 4, payload, false, false);
		}
	}

	pfree(dummy_jit.code_buf);
	dsm_detach(ram_seg);
}

/* ================================================================
 * JIT Diagnostic Tools
 *
 * Three SQL-callable tools for isolating JIT compiler bugs:
 *
 *   1. vm_jit_block_size(n)    — Set max instructions per JIT block.
 *                                 n=1 tests single-insn compilation.
 *                                 n=0 resets to compile-time default.
 *
 *   2. vm_jit_disable_op(op)   — Force opcode to interpreter path.
 *      vm_jit_enable_op(op)    — Re-enable JIT for opcode.
 *      vm_jit_disabled_ops()   — Show which opcodes are disabled.
 *
 *   3. vm_jit_compare(n)       — Run n instructions side-by-side:
 *                                 interpreter vs JIT (block_size=1).
 *                                 Returns the first divergence found.
 * ================================================================ */

/* ---- Tool 1: vm_jit_block_size(n INT) → TEXT ---- */

PG_FUNCTION_INFO_V1(rv64_vm_jit_block_size);
Datum
rv64_vm_jit_block_size(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_jit_block_size: no VM initialized");

	int32_t n = PG_GETARG_INT32(0);
	if (n < 0 || n > 4096)
		elog(ERROR, "vm_jit_block_size: n must be 0..4096 (0 = default)");

	/* Store request in shared memory — worker applies it before next run.
	 * We cannot dereference cpu->jit here because it's a worker-local pointer. */
	SpinLockAcquire(&vm_shared_state->mutex);
	vm_shared_state->jit_block_size_request = n;
	SpinLockRelease(&vm_shared_state->mutex);

	char buf[128];
	if (n == 0)
		snprintf(buf, sizeof(buf),
		         "JIT block size will reset to default on next step. Cache will be flushed.");
	else
		snprintf(buf, sizeof(buf),
		         "JIT block size will be set to %d on next step. Cache will be flushed.", n);

	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

/* ---- Tool 2: vm_jit_disable_op / enable_op / disabled_ops ---- */

PG_FUNCTION_INFO_V1(rv64_vm_jit_disable_op);
Datum
rv64_vm_jit_disable_op(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_jit_disable_op: no VM initialized");

	int32_t op = PG_GETARG_INT32(0);
	if (op < 0 || op > 127)
		elog(ERROR, "vm_jit_disable_op: opcode must be 0..127");

	SpinLockAcquire(&vm_shared_state->mutex);
	vm_shared_state->jit_disabled_ops[op / 64] |= (1ULL << (op % 64));
	vm_shared_state->jit_disabled_ops_dirty = true;
	SpinLockRelease(&vm_shared_state->mutex);

	/* Decode opcode name for human-readable output */
	const char *name = "???";
	switch (op) {
	case 0x03: name = "LOAD"; break;
	case 0x07: name = "FLW/FLD"; break;
	case 0x0F: name = "FENCE"; break;
	case 0x13: name = "OP-IMM"; break;
	case 0x17: name = "AUIPC"; break;
	case 0x1B: name = "OP-IMM-32"; break;
	case 0x23: name = "STORE"; break;
	case 0x27: name = "FSW/FSD"; break;
	case 0x2F: name = "AMO"; break;
	case 0x33: name = "OP"; break;
	case 0x37: name = "LUI"; break;
	case 0x3B: name = "OP-32"; break;
	case 0x43: name = "FMADD"; break;
	case 0x47: name = "FMSUB"; break;
	case 0x4B: name = "FNMSUB"; break;
	case 0x4F: name = "FNMADD"; break;
	case 0x53: name = "FP-OP"; break;
	case 0x63: name = "BRANCH"; break;
	case 0x67: name = "JALR"; break;
	case 0x6F: name = "JAL"; break;
	case 0x73: name = "SYSTEM"; break;
	}

	char buf[128];
	snprintf(buf, sizeof(buf),
	         "JIT will disable opcode 0x%02x (%s) on next step.", op, name);
	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

PG_FUNCTION_INFO_V1(rv64_vm_jit_enable_op);
Datum
rv64_vm_jit_enable_op(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_jit_enable_op: no VM initialized");

	int32_t op = PG_GETARG_INT32(0);
	if (op < 0 || op > 127)
		elog(ERROR, "vm_jit_enable_op: opcode must be 0..127");

	SpinLockAcquire(&vm_shared_state->mutex);
	vm_shared_state->jit_disabled_ops[op / 64] &= ~(1ULL << (op % 64));
	vm_shared_state->jit_disabled_ops_dirty = true;
	SpinLockRelease(&vm_shared_state->mutex);

	char buf[128];
	snprintf(buf, sizeof(buf),
	         "JIT will re-enable opcode 0x%02x on next step.", op);
	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

PG_FUNCTION_INFO_V1(rv64_vm_jit_disabled_ops);
Datum
rv64_vm_jit_disabled_ops(PG_FUNCTION_ARGS)
{
	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_jit_disabled_ops: no VM initialized");

	/* Read the bitmask from shared state (may be stale until next step applies it) */
	char buf[1024];
	int pos = 0;
	int count = 0;

	SpinLockAcquire(&vm_shared_state->mutex);
	uint64_t ops0 = vm_shared_state->jit_disabled_ops[0];
	uint64_t ops1 = vm_shared_state->jit_disabled_ops[1];
	SpinLockRelease(&vm_shared_state->mutex);

	for (int i = 0; i < 128; i++) {
		uint64_t mask = (i < 64) ? ops0 : ops1;
		if (mask & (1ULL << (i % 64))) {
			if (count > 0)
				pos += snprintf(buf + pos, sizeof(buf) - pos, ", ");
			pos += snprintf(buf + pos, sizeof(buf) - pos, "0x%02x", i);
			count++;
		}
	}

	if (count == 0)
		snprintf(buf, sizeof(buf), "none (all opcodes JIT-enabled)");

	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

/* ---- Tool 3: vm_jit_compare(n INT) → TABLE ----
 *
 * Runs up to n instructions. For each instruction that the JIT handles:
 *   1. Snapshot CPU state
 *   2. Execute via interpreter (1 insn)
 *   3. Record interpreter result
 *   4. Restore snapshot
 *   5. Execute via JIT (block_size=1)
 *   6. Compare all registers
 *   7. If mismatch → emit row, stop
 *   8. Restore to interpreter result (known-good) and continue
 *
 * Returns: (insn_num, pc, hex_insn, reg_name, interp_val, jit_val)
 * Empty result = no divergence found in n instructions.
 */

PG_FUNCTION_INFO_V1(rv64_vm_jit_compare);
Datum
rv64_vm_jit_compare(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *)fcinfo->resultinfo;
	TupleDesc tupdesc;
	Tuplestorestate *tstore;
	MemoryContext per_query_ctx, old_ctx;

	ensure_worker_attached();
	if (!vm_shared_state || !vm_shared_state->worker_ready)
		elog(ERROR, "vm_jit_compare: no VM initialized");

	ensure_frontend_ram();

	struct rv64_cpu *cpu = &vm_shared_state->cpu;
	/* cpu->jit is a worker-local pointer — we cannot dereference it from the
	 * frontend process.  vm_jit_compare needs to be reimplemented as an IPC
	 * command to the worker.  For now, error cleanly instead of SIGSEGV. */
	elog(ERROR, "vm_jit_compare: not yet supported in frontend/worker model "
	     "(cpu->jit is worker-local). Use vm_jit_block_size + vm_step to test.");

	int32_t max_insns = PG_GETARG_INT32(0);
	if (max_insns <= 0 || max_insns > 10000000)
		elog(ERROR, "vm_jit_compare: n must be 1..10000000");

	/* Optional second arg: block_size (default 1) */
	int32_t block_size = (PG_NARGS() > 1 && !PG_ARGISNULL(1))
	                   ? PG_GETARG_INT32(1) : 1;
	if (block_size < 1 || block_size > 64)
		elog(ERROR, "vm_jit_compare: block_size must be 1..64");

	/* Set up SRF return */
	if (rsi == NULL || !IsA(rsi, ReturnSetInfo))
		elog(ERROR, "vm_jit_compare: must be called in FROM clause");

	per_query_ctx = rsi->econtext->ecxt_per_query_memory;
	old_ctx = MemoryContextSwitchTo(per_query_ctx);

	tupdesc = CreateTemplateTupleDesc(6);
	TupleDescInitEntry(tupdesc, 1, "insn_num",  INT8OID,  -1, 0);
	TupleDescInitEntry(tupdesc, 2, "pc",        INT8OID,  -1, 0);
	TupleDescInitEntry(tupdesc, 3, "hex_insn",  TEXTOID,  -1, 0);
	TupleDescInitEntry(tupdesc, 4, "reg_name",  TEXTOID,  -1, 0);
	TupleDescInitEntry(tupdesc, 5, "interp_val", TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, 6, "jit_val",   TEXTOID,  -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	tstore = tuplestore_begin_heap(
		rsi->allowedModes & SFRM_Materialize_Random, false, work_mem);

	rsi->returnMode = SFRM_Materialize;
	rsi->setResult = tstore;
	rsi->setDesc = tupdesc;

	/* Save original JIT settings */
	int orig_max_block = cpu->jit->rt_max_block_insns;

	/* Set JIT block size for comparison */
	cpu->jit->rt_max_block_insns = block_size;
	jit_flush(cpu->jit);

	/*
	 * Lightweight architectural snapshot — only save/restore registers,
	 * PC, and CSRs. Device state, TLB, pointers (ram, jit, virtio) are
	 * left untouched to avoid pointer aliasing crashes.
	 */
	typedef struct {
		uint64_t x[32];
		uint64_t pc;
		uint64_t f[32];
		uint32_t fcsr;
		uint8_t  priv;
		/* M-mode CSRs */
		uint64_t mstatus, misa, medeleg, mideleg, mie, mtvec, mcounteren;
		uint64_t mscratch, mepc, mcause, mtval, mip;
		/* S-mode CSRs */
		uint64_t satp, sscratch, sepc, scause, stval, stvec, scounteren;
		uint64_t stimecmp, menvcfg;
		/* Counters */
		uint64_t cycle, instret;
	} arch_snap_t;

	/* Macro to copy architectural state between cpu and snap */
	#define SNAP_SAVE(dst, src) do { \
		memcpy((dst)->x, (src)->x, sizeof((dst)->x)); \
		(dst)->pc = (src)->pc; \
		memcpy((dst)->f, (src)->f, sizeof((dst)->f)); \
		(dst)->fcsr = (src)->fcsr; \
		(dst)->priv = (src)->priv; \
		(dst)->mstatus = (src)->mstatus; (dst)->misa = (src)->misa; \
		(dst)->medeleg = (src)->medeleg; (dst)->mideleg = (src)->mideleg; \
		(dst)->mie = (src)->mie; (dst)->mtvec = (src)->mtvec; \
		(dst)->mcounteren = (src)->mcounteren; \
		(dst)->mscratch = (src)->mscratch; (dst)->mepc = (src)->mepc; \
		(dst)->mcause = (src)->mcause; (dst)->mtval = (src)->mtval; \
		(dst)->mip = (src)->mip; \
		(dst)->satp = (src)->satp; (dst)->sscratch = (src)->sscratch; \
		(dst)->sepc = (src)->sepc; (dst)->scause = (src)->scause; \
		(dst)->stval = (src)->stval; (dst)->stvec = (src)->stvec; \
		(dst)->scounteren = (src)->scounteren; \
		(dst)->stimecmp = (src)->stimecmp; (dst)->menvcfg = (src)->menvcfg; \
		(dst)->cycle = (src)->cycle; (dst)->instret = (src)->instret; \
	} while(0)

	/* Restore snap back into live cpu (preserving pointers/devices) */
	#define SNAP_RESTORE(cpu, snap) do { \
		memcpy((cpu)->x, (snap)->x, sizeof((cpu)->x)); \
		(cpu)->pc = (snap)->pc; \
		memcpy((cpu)->f, (snap)->f, sizeof((cpu)->f)); \
		(cpu)->fcsr = (snap)->fcsr; \
		(cpu)->priv = (snap)->priv; \
		(cpu)->mstatus = (snap)->mstatus; (cpu)->misa = (snap)->misa; \
		(cpu)->medeleg = (snap)->medeleg; (cpu)->mideleg = (snap)->mideleg; \
		(cpu)->mie = (snap)->mie; (cpu)->mtvec = (snap)->mtvec; \
		(cpu)->mcounteren = (snap)->mcounteren; \
		(cpu)->mscratch = (snap)->mscratch; (cpu)->mepc = (snap)->mepc; \
		(cpu)->mcause = (snap)->mcause; (cpu)->mtval = (snap)->mtval; \
		(cpu)->mip = (snap)->mip; \
		(cpu)->satp = (snap)->satp; (cpu)->sscratch = (snap)->sscratch; \
		(cpu)->sepc = (snap)->sepc; (cpu)->scause = (snap)->scause; \
		(cpu)->stval = (snap)->stval; (cpu)->stvec = (snap)->stvec; \
		(cpu)->scounteren = (snap)->scounteren; \
		(cpu)->stimecmp = (snap)->stimecmp; (cpu)->menvcfg = (snap)->menvcfg; \
		(cpu)->cycle = (snap)->cycle; (cpu)->instret = (snap)->instret; \
	} while(0)

	arch_snap_t snap, interp_result;
	int divergence_found = 0;
	int64_t compared = 0, skipped = 0, total = 0;

	for (int64_t i = 0; i < max_insns && !divergence_found; i++) {
		total++;
		/* Skip if halted or error */
		if (cpu->halted || cpu->error)
			break;

		/* Snapshot architectural state */
		SNAP_SAVE(&snap, cpu);

		/* --- Run interpreter for block_size instructions --- */
		int interp_ok = 1;
		for (int s = 0; s < block_size; s++) {
			if (cpu->halted || cpu->error) { interp_ok = 0; break; }
			if (rv64_step(cpu) < 0) { interp_ok = 0; break; }
		}
		if (!interp_ok)
			break;

		/* Save interpreter result */
		SNAP_SAVE(&interp_result, cpu);

		/* --- Restore snapshot and run JIT --- */
		uint64_t saved_pc = snap.pc;
		SNAP_RESTORE(cpu, &snap);

		/* Try JIT execution */
		int jit_ran = jit_exec(cpu);

		if (jit_ran <= 0) {
			/* JIT couldn't handle this instruction — interpreter-only,
			 * no comparison possible. Restore interpreter result. */
			SNAP_RESTORE(cpu, &interp_result);
			skipped++;
			continue;
		}
		compared++;

		/* --- Compare all registers --- */
		static const char *reg_names[] = {
			"x0","x1/ra","x2/sp","x3/gp","x4/tp","x5/t0","x6/t1","x7/t2",
			"x8/s0","x9/s1","x10/a0","x11/a1","x12/a2","x13/a3","x14/a4","x15/a5",
			"x16/a6","x17/a7","x18/s2","x19/s3","x20/s4","x21/s5","x22/s6","x23/s7",
			"x24/s8","x25/s9","x26/s10","x27/s11","x28/t3","x29/t4","x30/t5","x31/t6"
		};

		/* Get the instruction hex for display */
		uint64_t insn_val = 0;
		if (saved_pc >= RAM_BASE && saved_pc < RAM_BASE + cpu->ram_size) {
			uint8_t *ram = get_guest_ram_ptr();
			if (ram) memcpy(&insn_val, ram + (saved_pc - RAM_BASE), 4);
		}
		char hex_buf[16];
		snprintf(hex_buf, sizeof(hex_buf), "0x%08x", (uint32_t)insn_val);

		/* Compare integer registers */
		for (int r = 0; r < 32; r++) {
			if (cpu->x[r] != interp_result.x[r]) {
				Datum vals[6];
				bool nulls[6] = {false};
				char iv[32], jv[32];

				snprintf(iv, sizeof(iv), "0x%016lx",
				         (unsigned long)interp_result.x[r]);
				snprintf(jv, sizeof(jv), "0x%016lx",
				         (unsigned long)cpu->x[r]);

				vals[0] = Int64GetDatum(i);
				vals[1] = Int64GetDatum((int64_t)saved_pc);
				vals[2] = CStringGetTextDatum(hex_buf);
				vals[3] = CStringGetTextDatum(reg_names[r]);
				vals[4] = CStringGetTextDatum(iv);
				vals[5] = CStringGetTextDatum(jv);

				tuplestore_putvalues(tstore, tupdesc, vals, nulls);
				divergence_found = 1;
			}
		}

		/* Compare PC */
		if (cpu->pc != interp_result.pc) {
			Datum vals[6];
			bool nulls[6] = {false};
			char iv[32], jv[32];

			snprintf(iv, sizeof(iv), "0x%016lx",
			         (unsigned long)interp_result.pc);
			snprintf(jv, sizeof(jv), "0x%016lx",
			         (unsigned long)cpu->pc);

			vals[0] = Int64GetDatum(i);
			vals[1] = Int64GetDatum((int64_t)saved_pc);
			vals[2] = CStringGetTextDatum(hex_buf);
			vals[3] = CStringGetTextDatum("pc");
			vals[4] = CStringGetTextDatum(iv);
			vals[5] = CStringGetTextDatum(jv);

			tuplestore_putvalues(tstore, tupdesc, vals, nulls);
			divergence_found = 1;
		}

		/* Always advance using interpreter result (known-good) */
		SNAP_RESTORE(cpu, &interp_result);
	}

	#undef SNAP_SAVE
	#undef SNAP_RESTORE

	/* Restore original JIT settings */
	cpu->jit->rt_max_block_insns = orig_max_block;
	jit_flush(cpu->jit);

	ereport(NOTICE,
		(errmsg("vm_jit_compare: total=%lld compared=%lld skipped=%lld diverged=%d",
		        (long long)total, (long long)compared, (long long)skipped, divergence_found)));

	MemoryContextSwitchTo(old_ctx);
	PG_RETURN_NULL();
}
