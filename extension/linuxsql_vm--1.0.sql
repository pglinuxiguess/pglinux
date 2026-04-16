-- linuxsql_vm--1.0.sql
-- RISC-V VM as SQL-queryable state

-- Composite type for vm_state()
CREATE TYPE vm_state_t AS (
    -- CPU
    pc              BIGINT,
    priv            INT,
    halted          BOOLEAN,
    mip             BIGINT,
    mie             BIGINT,
    mideleg         BIGINT,
    mstatus         BIGINT,
    instret         BIGINT,
    mtime           BIGINT,
    error           BOOLEAN,
    -- PLIC
    plic_pending    INT,
    plic_enable_s   INT,
    plic_claim_s    INT,
    plic_threshold_s INT,
    -- Virtio
    virtio_status   INT,
    virtio_int_status INT,
    vq0_ready       INT,
    vq0_last_avail  INT,
    vq0_avail_idx   INT,
    vq0_used_idx    INT,
    -- Console
    console_bytes   INT
);

-- Performance measurement
CREATE TYPE vm_perf_t AS (
    wall_ms     FLOAT8,
    instructions BIGINT,
    mips        FLOAT8
);

-- Profiling counters
CREATE TYPE vm_profile_t AS (
    exec_cycles     BIGINT,
    wfi_cycles      BIGINT,
    irq_checks      BIGINT,
    irq_taken       BIGINT,
    mmu_walks       BIGINT,
    mmu_walk_steps  BIGINT,
    tlb_hits        BIGINT,
    tlb_misses      BIGINT
);

-- Instruction class distribution
CREATE TYPE vm_insn_stats_t AS (
    alu     BIGINT,
    load    BIGINT,
    store   BIGINT,
    branch  BIGINT,
    jump    BIGINT,
    csr     BIGINT,
    fpu     BIGINT,
    atomic  BIGINT,
    system  BIGINT,
    other   BIGINT
);

-- I/O stats
CREATE TYPE vm_io_stats_t AS (
    blk_reads           BIGINT,
    blk_writes          BIGINT,
    blk_bytes_read      BIGINT,
    blk_bytes_written   BIGINT
);

-- ---- Core Functions ----

CREATE FUNCTION vm_boot(
    fw TEXT, kernel TEXT, dtb TEXT, initrd TEXT, disk TEXT DEFAULT NULL
) RETURNS TEXT
AS 'linuxsql_vm', 'rv64_vm_boot' LANGUAGE C;

CREATE FUNCTION vm_step(cycles BIGINT DEFAULT 10000000)
RETURNS BIGINT
AS 'linuxsql_vm', 'rv64_vm_step' LANGUAGE C STRICT;

CREATE FUNCTION vm_state()
RETURNS vm_state_t
AS 'linuxsql_vm', 'rv64_vm_state' LANGUAGE C STRICT;

CREATE FUNCTION vm_send(input TEXT)
RETURNS void
AS 'linuxsql_vm', 'rv64_vm_send' LANGUAGE C STRICT;

CREATE FUNCTION vm_console()
RETURNS TEXT
AS 'linuxsql_vm', 'rv64_vm_console' LANGUAGE C STRICT;

CREATE FUNCTION vm_console_tail(n INT DEFAULT 500)
RETURNS TEXT
AS 'linuxsql_vm', 'rv64_vm_console_tail' LANGUAGE C STRICT;

-- ---- Profiling Functions ----

CREATE FUNCTION vm_perf(cycles BIGINT DEFAULT 100000000)
RETURNS vm_perf_t
AS 'linuxsql_vm', 'rv64_vm_perf' LANGUAGE C STRICT;

CREATE FUNCTION vm_profile()
RETURNS vm_profile_t
AS 'linuxsql_vm', 'rv64_vm_profile' LANGUAGE C STRICT;

CREATE FUNCTION vm_profile_guest()
RETURNS TABLE(idx int, pc bigint, ra bigint, fp bigint)
AS 'linuxsql_vm', 'rv64_vm_profile_guest' LANGUAGE C STRICT;

CREATE FUNCTION vm_profile_guest_reset()
RETURNS VOID
AS 'linuxsql_vm', 'rv64_vm_profile_guest_reset' LANGUAGE C STRICT;

-- ---- Disk Persistence ----

/*
 * vm_blocks stores dirty 4KB disk blocks written by the guest.
 * vm_boot() overlays these on top of rootfs.img, making PostgreSQL
 * the authoritative disk store after the first vm_disk_flush().
 */
CREATE TABLE IF NOT EXISTS vm_blocks (
    vm_id   BIGINT NOT NULL DEFAULT 1,
    blk_idx BIGINT NOT NULL,
    data    BYTEA  NOT NULL,
    PRIMARY KEY (vm_id, blk_idx)
);

/*
 * vm_disk_flush() — Write all dirty guest disk blocks to vm_blocks.
 * Call this after a session to persist disk state. Safe to call
 * repeatedly; only blocks written since the last flush are stored.
 */
CREATE FUNCTION vm_disk_flush()
RETURNS TEXT
AS 'linuxsql_vm', 'rv64_vm_disk_flush' LANGUAGE C STRICT;

-- ---- Asset Storage ----

/*
 * vm_assets stores the immutable boot assets (firmware, kernel, dtb, initrd, disk).
 * This eliminates the need for host filesystem access.
 */
CREATE TABLE IF NOT EXISTS vm_assets (
    asset_type  TEXT PRIMARY KEY,
    data_oid    OID  NOT NULL,
    size_bytes  BIGINT NOT NULL,
    loaded_at   TIMESTAMPTZ DEFAULT now()
);


/*
 * vm_boot(vm_id BIGINT DEFAULT 1, ram_size_mb INT DEFAULT 2048) -> TEXT
 * Boots the VM completely natively using assets loaded into `vm_assets`.
 * If vm_state has an active snapshot for this vm_id, resumes it identically.
 * Applies network bindings via virtio-net / libslirp.
 */
CREATE FUNCTION vm_boot(vm_id BIGINT DEFAULT 1, ram_size_mb INT DEFAULT 2048)
RETURNS TEXT
AS 'linuxsql_vm', 'rv64_vm_boot_from_db' LANGUAGE C;

/*
 * vm_state_save(vm_id BIGINT DEFAULT 1) -> TEXT
 * Persists CPU state and RAM to vm_state block natively.
 */
CREATE FUNCTION vm_state_save(vm_id BIGINT DEFAULT 1)
RETURNS TEXT
AS 'linuxsql_vm', 'rv64_vm_state_save' LANGUAGE C;

/*
 * vm_snapshot_create(src_id, dst_id, name)
 * Clone the RAM, CPU state, and Disk Blocks for instant multi-tenancy.
 */
CREATE OR REPLACE FUNCTION vm_snapshot_create(src_id BIGINT, dst_id BIGINT, snap_name TEXT)
RETURNS TEXT AS $$
DECLARE
    src_ram_oid OID;
    src_disk_oid OID;
    dst_ram_oid OID;
    dst_disk_oid OID;
BEGIN
    SELECT ram_oid, disk_oid INTO src_ram_oid, src_disk_oid
    FROM vm_state WHERE id = src_id;
    
    IF NOT FOUND THEN
        RAISE EXCEPTION 'Source VM % not found in vm_state', src_id;
    END IF;

    -- Clone OIDs using the highly optimized lo_from_bytea(lo_get) trick.
    -- (Warning: Requires the DB config to support large max_connections / shared buffers if OIDs reach multi-GB)
    SELECT lo_from_bytea(0, lo_get(src_ram_oid)) INTO dst_ram_oid;
    
    IF src_disk_oid != 0 THEN
    	SELECT lo_from_bytea(0, lo_get(src_disk_oid)) INTO dst_disk_oid;
    ELSE
    	dst_disk_oid := 0;
    END IF;

    -- Clone CPU and VM structure
    INSERT INTO vm_state (id, cpu_state, console, ram_oid, disk_oid, ram_size, disk_size, instret, pc, cycle)
    SELECT dst_id, cpu_state, console, dst_ram_oid, dst_disk_oid, ram_size, disk_size, instret, pc, cycle
    FROM vm_state WHERE id = src_id
    ON CONFLICT (id) DO UPDATE SET
        cpu_state = EXCLUDED.cpu_state, console = EXCLUDED.console,
        ram_oid = EXCLUDED.ram_oid, disk_oid = EXCLUDED.disk_oid,
        ram_size = EXCLUDED.ram_size, disk_size = EXCLUDED.disk_size,
        instret = EXCLUDED.instret, pc = EXCLUDED.pc, cycle = EXCLUDED.cycle;

    -- Clone isolated subset of dirty disk blocks
    DELETE FROM vm_blocks WHERE vm_id = dst_id;
    INSERT INTO vm_blocks (vm_id, blk_idx, data)
    SELECT dst_id, blk_idx, data FROM vm_blocks WHERE vm_id = src_id;

    RETURN 'Snapshot created: ' || snap_name || ' (ID: ' || dst_id || ')';
END;
$$ LANGUAGE plpgsql;

/*
 * vm_asset_load() — Reads a binary file from the host filesystem
 * and saves it into the vm_assets table as a large object.
 * Uses lo_import to handle files larger than 1GB (e.g. rootfs.img).
 */
CREATE FUNCTION vm_asset_load(p_asset_type TEXT, p_filepath TEXT)
RETURNS TEXT
LANGUAGE plpgsql AS $$
DECLARE
    new_oid OID;
    l_size BIGINT;
    lod INT;
BEGIN
    SELECT lo_import(p_filepath) INTO new_oid;
    
    -- Measure size using large object API
    lod := lo_open(new_oid, x'40000'::int); -- INV_READ
    l_size := lo_lseek64(lod, 0, 2);      -- SEEK_END
    PERFORM lo_close(lod);

    INSERT INTO vm_assets (asset_type, data_oid, size_bytes)
    VALUES (p_asset_type, new_oid, l_size)
    ON CONFLICT (asset_type) DO UPDATE SET
        data_oid = EXCLUDED.data_oid,
        size_bytes = EXCLUDED.size_bytes,
        loaded_at = now();
        
    RETURN format('Loaded %s from %s (OID: %s, Size: %s bytes)', p_asset_type, p_filepath, new_oid, l_size);
END;
$$;


CREATE FUNCTION vm_insn_stats()
RETURNS vm_insn_stats_t
AS 'linuxsql_vm', 'rv64_vm_insn_stats' LANGUAGE C STRICT;

CREATE FUNCTION vm_io_stats()
RETURNS vm_io_stats_t
AS 'linuxsql_vm', 'rv64_vm_io_stats' LANGUAGE C STRICT;

-- JIT compiler stats
CREATE TYPE vm_jit_stats_t AS (
    enabled         BOOLEAN,
    cache_hits      BIGINT,
    compiles        BIGINT,
    compile_fails   BIGINT,
    jit_insns       BIGINT,
    jit_coverage    FLOAT8
);

CREATE FUNCTION vm_jit_stats()
RETURNS vm_jit_stats_t
AS 'linuxsql_vm', 'rv64_vm_jit_stats' LANGUAGE C STRICT;

CREATE FUNCTION vm_jit_set(enabled BOOLEAN) RETURNS TEXT
AS 'linuxsql_vm', 'rv64_vm_jit_set' LANGUAGE C STRICT;

CREATE FUNCTION vm_profile_host(pid INT, seconds INT DEFAULT 10)
RETURNS TEXT
AS 'linuxsql_vm', 'rv64_vm_profile_host' LANGUAGE C STRICT;

-- ---- Virtual Machine Management ----

CREATE FUNCTION vm_console_clear()
RETURNS VOID
AS 'linuxsql_vm', 'rv64_vm_console_clear' LANGUAGE C;

CREATE FUNCTION vm_type(input TEXT)
RETURNS VOID
AS 'linuxsql_vm', 'rv64_vm_type' LANGUAGE C STRICT;

-- Sequence for unique sentinel IDs
CREATE SEQUENCE vm_exec_seq;

-- ---- High-level Execution ----

/*
 * vm_exec(command TEXT, timeout_cycles BIGINT) → TEXT
 *
 * Execute a shell command in the guest and return its stdout.
 * Uses a unique sentinel to detect when the command completes.
 * Output is stripped of the echoed command, sentinel, and prompt.
 */
CREATE FUNCTION vm_exec(
    cmd TEXT,
    timeout_cycles BIGINT DEFAULT 200000000
) RETURNS TEXT
LANGUAGE plpgsql AS $$
DECLARE
    sentinel TEXT;
    full_cmd TEXT;
    batch_size BIGINT := 2000000;
    cycles_used BIGINT := 0;
    console_before INT;
    buf TEXT;
    sentinel_pos INT;
    lines TEXT[];
    result_lines TEXT[] := '{}';
    line TEXT;
BEGIN
    -- Build unique sentinel
    sentinel := '__SQLDONE_' || nextval('vm_exec_seq') || '__';

    -- Snapshot console length before we send anything
    console_before := length(vm_console());

    -- Send command with sentinel appended
    full_cmd := cmd || '; echo ' || sentinel;
    PERFORM vm_type(full_cmd || CHR(13));

    -- Run in batches until we see the sentinel in new output.
    -- The sentinel appears in the echoed command (potentially split by
    -- line wrapping) and as actual command output. We wait for the
    -- actual output sentinel by checking if the prompt appears after
    -- the LAST sentinel occurrence.
    LOOP
        PERFORM vm_step(batch_size);
        cycles_used := cycles_used + batch_size;

        buf := substring(vm_console() FROM console_before + 1);

        -- Look for sentinel followed eventually by a prompt
        IF position(sentinel IN buf) > 0 THEN
            -- After the last sentinel, should be prompt or newline+prompt
            DECLARE
                last_sent_pos INT;
                after TEXT;
            BEGIN
                -- Find last occurrence via reverse
                last_sent_pos := length(buf)
                    - position(reverse(sentinel) IN reverse(buf))
                    - length(sentinel) + 2;
                after := substring(buf FROM last_sent_pos + length(sentinel));
                -- If we see the prompt after the last sentinel, we're done
                -- Match both 'linuxsql# ' (profile'd shell) and bare '# ' (dash)
                EXIT WHEN after ~ '(linuxsql)?#\s';
                -- Or if there are 2+ occurrences (echo wasn't split)
                DECLARE
                    first_pos INT := position(sentinel IN buf);
                BEGIN
                    EXIT WHEN first_pos < last_sent_pos;
                END;
            END;
        END IF;

        EXIT WHEN cycles_used >= timeout_cycles;
    END LOOP;

    -- Extract new output, trim at last sentinel, skip echoed command
    buf := substring(vm_console() FROM console_before + 1);
    buf := replace(buf, E'\r', '');

    -- Find the last sentinel (the real output one)
    DECLARE
        last_pos INT;
    BEGIN
        last_pos := length(buf)
            - position(reverse(sentinel) IN reverse(buf))
            - length(sentinel) + 2;
        IF last_pos > 0 AND last_pos <= length(buf) THEN
            buf := left(buf, last_pos - 1);
        END IF;
    END;

    -- Skip the echoed command. The echo is always the text of full_cmd
    -- (with \n added) at the start of buf. Skip all lines up to and
    -- including the line that contains the full_cmd's last few chars.
    -- Simpler: just skip character count equal to length(full_cmd) + margins,
    -- then find the next \n.
    -- Even simpler: find the first line that starts with a real path or output,
    -- not with the command text.
    lines := regexp_split_to_array(buf, E'\n');

    -- The echoed command occupies ceil(length(full_cmd) / 80) lines
    -- (terminal is ~80 cols wide). Skip that many lines from the front.
    DECLARE
        skip_lines INT := ceil(length(full_cmd)::float / 80.0);
    BEGIN
        FOR i IN 1..coalesce(array_length(lines, 1), 0) LOOP
            IF i <= skip_lines THEN
                CONTINUE; -- skip echoed command lines
            END IF;
            line := trim(lines[i]);
            CONTINUE WHEN line ~ '^\s*(linuxsql)?#\s*$';
            CONTINUE WHEN line = '';
            result_lines := array_append(result_lines, line);
        END LOOP;
    END;

    RETURN array_to_string(result_lines, E'\n');
END;
$$;

-- ---- File I/O Wrappers ----

CREATE FUNCTION vm_read_file(filepath TEXT) RETURNS TEXT AS $$
    SELECT vm_exec('cat ' || filepath);
$$ LANGUAGE SQL;

CREATE FUNCTION vm_write_file(filepath TEXT, val TEXT) RETURNS TEXT AS $$
    SELECT vm_exec('echo ' || quote_literal(val) || ' > ' || filepath || ' && echo OK');
$$ LANGUAGE SQL;

CREATE FUNCTION vm_sysctl_read(key TEXT) RETURNS TEXT AS $$
    SELECT vm_exec('cat /proc/sys/' || replace(key, '.', '/'));
$$ LANGUAGE SQL;

CREATE FUNCTION vm_sysctl_write(key TEXT, val TEXT) RETURNS TEXT AS $$
    SELECT vm_exec('echo ' || quote_literal(val) || ' > /proc/sys/' || replace(key, '.', '/') || ' && echo OK');
$$ LANGUAGE SQL;

/*
 * vm_proc() → TABLE(host, path, content)
 *
 * Dump all readable /proc files as rows.
 * Two-pass: list files, then read each one.
 */
CREATE FUNCTION vm_proc()
RETURNS TABLE(host TEXT, path TEXT, content TEXT)
LANGUAGE plpgsql AS $$
DECLARE
    file_list TEXT;
    files TEXT[];
    f TEXT;
    file_content TEXT;
BEGIN
    file_list := vm_exec(
        'find /proc -maxdepth 2 -type f '
        || '-not -path ''/proc/kcore'' '
        || '-not -path ''/proc/kmsg'' '
        || '-not -path ''/proc/sysrq-trigger'' '
        || '-not -path ''/proc/*/mem'' '
        || '-not -path ''/proc/*/pagemap'' '
        || '-not -path ''/proc/*/clear_refs'' '
        || '2>/dev/null'
    );

    files := regexp_split_to_array(file_list, E'\n');

    FOR i IN 1..coalesce(array_length(files, 1), 0) LOOP
        f := trim(files[i]);
        CONTINUE WHEN f = '' OR f IS NULL;
        BEGIN
            file_content := vm_exec('cat ' || f || ' 2>/dev/null | head -c 4096');
            IF file_content IS NOT NULL AND file_content != '' THEN
                host := 'linuxsql-vm';
                path := f;
                content := file_content;
                RETURN NEXT;
            END IF;
        EXCEPTION WHEN OTHERS THEN
            NULL;
        END;
    END LOOP;
END;
$$;

-- ============================================================
-- STRUCTURED /proc VIEWS
-- Parse raw procfs text into proper typed SQL columns.
-- ============================================================

-- ---- Process Table (like ps aux) ----

CREATE FUNCTION vm_ps()
RETURNS TABLE(
    pid INT,
    ppid INT,
    name TEXT,
    state TEXT,
    uid INT,
    threads INT,
    vmrss_kb INT,
    vmsize_kb INT,
    fds INT,
    cmdline TEXT
)
LANGUAGE plpgsql AS $$
DECLARE
    pids_raw TEXT;
    pids TEXT[];
    p INT;
    status_raw TEXT;
    m TEXT[];
BEGIN
    -- List PIDs from SQL side (avoids busybox glob crash)
    pids_raw := vm_exec('ls /proc/ | grep "^[0-9]"');
    pids := regexp_split_to_array(pids_raw, E'\n');

    FOR i IN 1..coalesce(array_length(pids, 1), 0) LOOP
        BEGIN
            p := trim(pids[i])::INT;
        EXCEPTION WHEN OTHERS THEN
            CONTINUE;
        END;

        BEGIN
            status_raw := vm_exec('cat /proc/' || p || '/status 2>/dev/null');
            IF status_raw IS NULL OR status_raw = '' THEN CONTINUE; END IF;

            pid := p;

            m := regexp_match(status_raw, 'Name:\s+(\S+)');
            name := coalesce(trim(m[1]), '?');

            m := regexp_match(status_raw, 'State:\s+(\S)');
            state := CASE m[1]
                WHEN 'R' THEN 'running'
                WHEN 'S' THEN 'sleeping'
                WHEN 'D' THEN 'disk_sleep'
                WHEN 'Z' THEN 'zombie'
                WHEN 'T' THEN 'stopped'
                WHEN 'I' THEN 'idle'
                ELSE coalesce(m[1], '?')
            END;

            m := regexp_match(status_raw, 'PPid:\s+(\d+)');
            ppid := coalesce(m[1]::INT, 0);

            m := regexp_match(status_raw, 'Uid:\s+(\d+)');
            uid := coalesce(m[1]::INT, 0);

            m := regexp_match(status_raw, 'Threads:\s+(\d+)');
            threads := coalesce(m[1]::INT, 1);

            m := regexp_match(status_raw, 'VmRSS:\s+(\d+)');
            vmrss_kb := coalesce(m[1]::INT, 0);

            m := regexp_match(status_raw, 'VmSize:\s+(\d+)');
            vmsize_kb := coalesce(m[1]::INT, 0);

            -- Fd count and cmdline in separate calls
            BEGIN
                fds := coalesce(trim(vm_exec(
                    'ls /proc/' || p || '/fd 2>/dev/null | wc -l'
                ))::INT, 0);
            EXCEPTION WHEN OTHERS THEN
                fds := 0;
            END;

            BEGIN
                cmdline := trim(vm_exec(
                    'tr "\0" " " < /proc/' || p || '/cmdline 2>/dev/null'
                ));
            EXCEPTION WHEN OTHERS THEN
                cmdline := '';
            END;

            RETURN NEXT;
        EXCEPTION WHEN OTHERS THEN
            CONTINUE;
        END;
    END LOOP;
END;
$$;


-- ---- Memory Info ----

CREATE FUNCTION vm_meminfo()
RETURNS TABLE(key TEXT, value_kb BIGINT, pct_of_total NUMERIC(5,1))
LANGUAGE plpgsql AS $$
DECLARE
    raw TEXT;
    lines TEXT[];
    m TEXT[];
    total_kb BIGINT := 0;
BEGIN
    raw := vm_exec('cat /proc/meminfo');
    lines := regexp_split_to_array(raw, E'\n');

    -- First pass: find MemTotal
    FOR i IN 1..coalesce(array_length(lines, 1), 0) LOOP
        m := regexp_match(lines[i], 'MemTotal:\s+(\d+)');
        IF m IS NOT NULL THEN total_kb := m[1]::BIGINT; EXIT; END IF;
    END LOOP;

    -- Second pass: emit all rows
    FOR i IN 1..coalesce(array_length(lines, 1), 0) LOOP
        m := regexp_match(lines[i], '^(\S+):\s+(\d+)');
        IF m IS NULL THEN CONTINUE; END IF;
        key := m[1];
        value_kb := m[2]::BIGINT;
        IF total_kb > 0 THEN
            pct_of_total := round(value_kb::NUMERIC / total_kb * 100, 1);
        ELSE
            pct_of_total := NULL;
        END IF;
        RETURN NEXT;
    END LOOP;
END;
$$;


-- ---- Mount Table ----

CREATE FUNCTION vm_mounts()
RETURNS TABLE(device TEXT, mountpoint TEXT, fstype TEXT, options TEXT)
LANGUAGE plpgsql AS $$
DECLARE
    raw TEXT;
    lines TEXT[];
    parts TEXT[];
BEGIN
    raw := vm_exec('cat /proc/mounts');
    lines := regexp_split_to_array(raw, E'\n');

    FOR i IN 1..coalesce(array_length(lines, 1), 0) LOOP
        parts := regexp_split_to_array(trim(lines[i]), '\s+');
        CONTINUE WHEN coalesce(array_length(parts, 1), 0) < 4;
        device     := parts[1];
        mountpoint := parts[2];
        fstype     := parts[3];
        options    := parts[4];
        RETURN NEXT;
    END LOOP;
END;
$$;


-- ---- Network Interfaces ----

CREATE FUNCTION vm_net_dev()
RETURNS TABLE(iface TEXT, rx_bytes BIGINT, rx_packets BIGINT, tx_bytes BIGINT, tx_packets BIGINT)
LANGUAGE plpgsql AS $$
DECLARE
    raw TEXT;
    lines TEXT[];
    m TEXT[];
    parts TEXT[];
BEGIN
    raw := vm_exec('cat /proc/net/dev');
    lines := regexp_split_to_array(raw, E'\n');

    FOR i IN 1..coalesce(array_length(lines, 1), 0) LOOP
        m := regexp_match(lines[i], '^\s*(\S+):\s+(.+)$');
        IF m IS NULL THEN CONTINUE; END IF;
        iface := m[1];
        parts := regexp_split_to_array(trim(m[2]), '\s+');
        CONTINUE WHEN coalesce(array_length(parts, 1), 0) < 10;
        rx_bytes   := parts[1]::BIGINT;
        rx_packets := parts[2]::BIGINT;
        tx_bytes   := parts[9]::BIGINT;
        tx_packets := parts[10]::BIGINT;
        RETURN NEXT;
    END LOOP;
END;
$$;


-- ---- File Descriptors ----

CREATE FUNCTION vm_proc_fds(target_pid INT DEFAULT NULL)
RETURNS TABLE(pid INT, fd INT, target TEXT)
LANGUAGE plpgsql AS $$
DECLARE
    raw TEXT;
    lines TEXT[];
    m TEXT[];
    pids_raw TEXT;
    pids TEXT[];
    p INT;
BEGIN
    IF target_pid IS NOT NULL THEN
        pids := ARRAY[target_pid::TEXT];
    ELSE
        pids_raw := vm_exec('ls /proc/ | grep "^[0-9]"');
        pids := regexp_split_to_array(pids_raw, E'\n');
    END IF;

    FOR i IN 1..coalesce(array_length(pids, 1), 0) LOOP
        p := trim(pids[i])::INT;
        BEGIN
            -- Use ls + readlink to get fd targets without ANSI escape codes
            raw := vm_exec(
                'ls /proc/' || p || '/fd 2>/dev/null | while read f; do '
                || 'echo "$f $(readlink /proc/' || p || '/fd/$f 2>/dev/null)"; '
                || 'done'
            );
            lines := regexp_split_to_array(raw, E'\n');
            FOR j IN 1..coalesce(array_length(lines, 1), 0) LOOP
                m := regexp_match(lines[j], '^(\d+)\s+(.+)$');
                IF m IS NOT NULL THEN
                    pid := p;
                    fd := m[1]::INT;
                    target := m[2];
                    RETURN NEXT;
                END IF;
            END LOOP;
        EXCEPTION WHEN OTHERS THEN
            NULL;
        END;
    END LOOP;
END;
$$;


-- ---- CPU Info ----

CREATE FUNCTION vm_cpuinfo()
RETURNS TABLE(processor INT, key TEXT, value TEXT)
LANGUAGE plpgsql AS $$
DECLARE
    raw TEXT;
    lines TEXT[];
    m TEXT[];
    cur_proc INT := 0;
BEGIN
    raw := vm_exec('cat /proc/cpuinfo');
    lines := regexp_split_to_array(raw, E'\n');

    FOR i IN 1..coalesce(array_length(lines, 1), 0) LOOP
        m := regexp_match(lines[i], '^(\S[^:]+\S)\s*:\s*(.*)$');
        IF m IS NULL THEN CONTINUE; END IF;
        key := trim(m[1]);
        value := trim(m[2]);
        IF key = 'processor' THEN cur_proc := value::INT; END IF;
        processor := cur_proc;
        RETURN NEXT;
    END LOOP;
END;
$$;


-- ---- Load Average ----

CREATE FUNCTION vm_loadavg()
RETURNS TABLE(avg_1min NUMERIC, avg_5min NUMERIC, avg_15min NUMERIC,
              running INT, total INT, last_pid INT)
LANGUAGE plpgsql AS $$
DECLARE
    raw TEXT;
    parts TEXT[];
    running_total TEXT[];
BEGIN
    raw := vm_exec('cat /proc/loadavg');
    parts := regexp_split_to_array(trim(raw), '\s+');
    IF coalesce(array_length(parts, 1), 0) < 5 THEN RETURN; END IF;

    avg_1min  := parts[1]::NUMERIC;
    avg_5min  := parts[2]::NUMERIC;
    avg_15min := parts[3]::NUMERIC;
    running_total := string_to_array(parts[4], '/');
    running  := running_total[1]::INT;
    total    := running_total[2]::INT;
    last_pid := parts[5]::INT;
    RETURN NEXT;
END;
$$;


-- ---- Uptime ----

CREATE FUNCTION vm_uptime()
RETURNS TABLE(uptime_seconds NUMERIC, idle_seconds NUMERIC, uptime_pretty TEXT)
LANGUAGE plpgsql AS $$
DECLARE
    raw TEXT;
    parts TEXT[];
    secs NUMERIC;
    h INT; m INT; s INT;
BEGIN
    raw := vm_exec('cat /proc/uptime');
    parts := regexp_split_to_array(trim(raw), '\s+');
    IF coalesce(array_length(parts, 1), 0) < 2 THEN RETURN; END IF;

    uptime_seconds := parts[1]::NUMERIC;
    idle_seconds   := parts[2]::NUMERIC;

    secs := uptime_seconds;
    h := floor(secs / 3600)::INT;
    m := floor((secs - h * 3600) / 60)::INT;
    s := floor(secs - h * 3600 - m * 60)::INT;
    uptime_pretty := format('%sh %sm %ss', h, m, s);
    RETURN NEXT;
END;
$$;


-- ---- Process Tree (recursive CTE) ----

CREATE FUNCTION vm_proc_tree()
RETURNS TABLE(tree_display TEXT, pid INT, ppid INT, name TEXT, state TEXT, vmrss_kb INT)
LANGUAGE SQL AS $$
    WITH RECURSIVE procs AS (
        SELECT pid, ppid, name, state, vmrss_kb FROM vm_ps()
    ),
    tree AS (
        SELECT 0 AS depth, pid, ppid, name, state, vmrss_kb
        FROM procs WHERE ppid = 0
        UNION ALL
        SELECT t.depth + 1, p.pid, p.ppid, p.name, p.state, p.vmrss_kb
        FROM procs p JOIN tree t ON p.ppid = t.pid
    )
    SELECT
        repeat('  ', depth) || '├─ ' || name,
        pid, ppid, name, state, vmrss_kb
    FROM tree ORDER BY depth, pid;
$$;


-- ---- Write Operations ----

CREATE FUNCTION vm_kill(target_pid INT, sig INT DEFAULT 15) RETURNS TEXT AS $$
    SELECT vm_exec('kill -' || sig || ' ' || target_pid || ' 2>&1 && echo OK');
$$ LANGUAGE SQL;

CREATE FUNCTION vm_oom_adj(target_pid INT, score INT) RETURNS TEXT AS $$
    SELECT vm_exec('echo ' || score || ' > /proc/' || target_pid || '/oom_score_adj 2>&1 && echo OK');
$$ LANGUAGE SQL;

CREATE FUNCTION vm_hostname(new_name TEXT DEFAULT NULL) RETURNS TEXT AS $$
    SELECT CASE
        WHEN new_name IS NULL THEN vm_exec('hostname')
        ELSE vm_exec('hostname ' || quote_literal(new_name) || ' && hostname')
    END;
$$ LANGUAGE SQL;


-- ============================================================
-- DEBUG INSTRUMENTATION
-- Memory reads, watchpoints, breakpoints, register dumps.
-- These are SQL tools for debugging the RISC-V emulator.
-- ============================================================

-- ---- Memory Read/Write ----

CREATE FUNCTION vm_peek(addr BIGINT, size INT DEFAULT 8)
RETURNS BIGINT
AS 'linuxsql_vm', 'rv64_vm_peek' LANGUAGE C;

CREATE FUNCTION vm_poke(addr BIGINT, value BIGINT, size INT DEFAULT 8)
RETURNS VOID
AS 'linuxsql_vm', 'rv64_vm_poke' LANGUAGE C;

-- Convenience: hex dump of N bytes starting at addr
CREATE FUNCTION vm_peek_hex(start_addr BIGINT, num_bytes INT DEFAULT 64)
RETURNS TABLE(addr TEXT, hex TEXT, ascii TEXT)
LANGUAGE plpgsql AS $$
DECLARE
    i INT;
    line_addr BIGINT;
    hex_str TEXT;
    ascii_str TEXT;
    b INT;
    ch CHAR;
BEGIN
    FOR i IN 0..(num_bytes - 1) BY 16 LOOP
        line_addr := start_addr + i;
        hex_str := '';
        ascii_str := '';
        FOR j IN 0..15 LOOP
            EXIT WHEN i + j >= num_bytes;
            BEGIN
                b := vm_peek(line_addr + j, 1)::INT & 255;
                hex_str := hex_str || lpad(to_hex(b), 2, '0') || ' ';
                IF b >= 32 AND b < 127 THEN
                    ascii_str := ascii_str || chr(b);
                ELSE
                    ascii_str := ascii_str || '.';
                END IF;
            EXCEPTION WHEN OTHERS THEN
                hex_str := hex_str || '?? ';
                ascii_str := ascii_str || '?';
            END;
        END LOOP;
        addr := '0x' || lpad(to_hex(line_addr), 16, '0');
        hex := rtrim(hex_str);
        ascii := ascii_str;
        RETURN NEXT;
    END LOOP;
END;
$$;


-- ---- Memory Watchpoint ----

CREATE FUNCTION vm_watchpoint(addr BIGINT, size INT DEFAULT 4)
RETURNS TEXT
AS 'linuxsql_vm', 'rv64_vm_watchpoint' LANGUAGE C;

CREATE FUNCTION vm_watch_log()
RETURNS TABLE(hit_num INT, pc BIGINT, addr BIGINT, value BIGINT, size INT, instret BIGINT)
AS 'linuxsql_vm', 'rv64_vm_watch_log' LANGUAGE C;

-- Convenience: formatted watchpoint log with hex addresses
CREATE FUNCTION vm_watch_log_hex()
RETURNS TABLE(hit TEXT, pc_hex TEXT, addr_hex TEXT, value_hex TEXT, size INT, instret BIGINT)
LANGUAGE SQL AS $$
    SELECT
        '#' || hit_num::TEXT,
        '0x' || lpad(to_hex(pc), 16, '0'),
        '0x' || lpad(to_hex(addr), 16, '0'),
        '0x' || lpad(to_hex(value), 16, '0'),
        size,
        instret
    FROM vm_watch_log();
$$;


-- ---- PC Breakpoint ----

CREATE FUNCTION vm_breakpoint(pc BIGINT, halt BOOLEAN DEFAULT false)
RETURNS TEXT
AS 'linuxsql_vm', 'rv64_vm_breakpoint' LANGUAGE C;

CREATE FUNCTION vm_break_log()
RETURNS TABLE(hit_num INT, pc BIGINT, sp BIGINT, instret BIGINT, x_hex TEXT, f_hex TEXT)
AS 'linuxsql_vm', 'rv64_vm_break_log' LANGUAGE C;


-- ---- Register Dump ----

CREATE TYPE vm_regs_t AS (
    pc      BIGINT,
    sp      BIGINT,
    instret BIGINT,
    x_hex   TEXT,
    f_hex   TEXT
);

CREATE FUNCTION vm_regs()
RETURNS vm_regs_t
AS 'linuxsql_vm', 'rv64_vm_regs' LANGUAGE C STRICT;

-- Convenience: parse x_hex into individual named registers
CREATE FUNCTION vm_regs_named()
RETURNS TABLE(reg TEXT, value TEXT)
LANGUAGE plpgsql AS $$
DECLARE
    r vm_regs_t;
    parts TEXT[];
    names TEXT[] := ARRAY[
        'x0/zero', 'x1/ra', 'x2/sp', 'x3/gp', 'x4/tp',
        'x5/t0', 'x6/t1', 'x7/t2', 'x8/s0', 'x9/s1',
        'x10/a0', 'x11/a1', 'x12/a2', 'x13/a3', 'x14/a4', 'x15/a5',
        'x16/a6', 'x17/a7', 'x18/s2', 'x19/s3', 'x20/s4', 'x21/s5',
        'x22/s6', 'x23/s7', 'x24/s8', 'x25/s9', 'x26/s10', 'x27/s11',
        'x28/t3', 'x29/t4', 'x30/t5', 'x31/t6'
    ];
    fp_names TEXT[] := ARRAY[
        'f0/ft0', 'f1/ft1', 'f2/ft2', 'f3/ft3', 'f4/ft4', 'f5/ft5', 'f6/ft6', 'f7/ft7',
        'f8/fs0', 'f9/fs1',
        'f10/fa0', 'f11/fa1', 'f12/fa2', 'f13/fa3', 'f14/fa4', 'f15/fa5', 'f16/fa6', 'f17/fa7',
        'f18/fs2', 'f19/fs3', 'f20/fs4', 'f21/fs5', 'f22/fs6', 'f23/fs7',
        'f24/fs8', 'f25/fs9', 'f26/fs10', 'f27/fs11',
        'f28/ft8', 'f29/ft9', 'f30/ft10', 'f31/ft11'
    ];
BEGIN
    SELECT * INTO r FROM vm_regs();

    -- PC and SP first
    reg := 'pc'; value := '0x' || lpad(to_hex(r.pc), 16, '0'); RETURN NEXT;
    reg := 'sp'; value := '0x' || lpad(to_hex(r.sp), 16, '0'); RETURN NEXT;
    reg := 'instret'; value := r.instret::TEXT; RETURN NEXT;

    -- Integer registers
    parts := regexp_split_to_array(trim(r.x_hex), '\s+');
    FOR i IN 1..LEAST(array_length(parts,1), 32) LOOP
        reg := names[i];
        value := '0x' || parts[i];
        RETURN NEXT;
    END LOOP;

    -- FP registers
    parts := regexp_split_to_array(trim(r.f_hex), '\s+');
    FOR i IN 1..LEAST(array_length(parts,1), 32) LOOP
        reg := fp_names[i];
        value := '0x' || parts[i];
        RETURN NEXT;
    END LOOP;
END;
$$;


-- ============================================================
-- ADVANCED DEBUG TOOLS
-- Memory search, step-until, halt-on-break, console correlation.
-- ============================================================

-- ---- Memory Search ----

CREATE FUNCTION vm_find(pattern BYTEA, start_addr BIGINT, length INT)
RETURNS BIGINT
AS 'linuxsql_vm', 'rv64_vm_find' LANGUAGE C;

-- Convenience: find a text string in guest memory
CREATE FUNCTION vm_find_string(needle TEXT, start_addr BIGINT, length INT)
RETURNS TEXT
LANGUAGE SQL AS $$
    SELECT CASE
        WHEN vm_find(needle::BYTEA, start_addr, length) IS NOT NULL
        THEN '0x' || lpad(to_hex(vm_find(needle::BYTEA, start_addr, length)), 16, '0')
        ELSE NULL
    END;
$$;


-- ---- Execution Control ----

CREATE FUNCTION vm_resume()
RETURNS TEXT
AS 'linuxsql_vm', 'rv64_vm_resume' LANGUAGE C;

CREATE TYPE vm_step_until_t AS (
    found       BOOLEAN,
    cycles_used BIGINT,
    instret     BIGINT,
    snippet     TEXT
);

CREATE FUNCTION vm_step_until(pattern TEXT, max_cycles BIGINT DEFAULT 500000000)
RETURNS vm_step_until_t
AS 'linuxsql_vm', 'rv64_vm_step_until' LANGUAGE C;


-- ---- Console-Instret Correlation ----

CREATE FUNCTION vm_console_instret()
RETURNS TABLE(line_num INT, instret BIGINT, line_text TEXT)
AS 'linuxsql_vm', 'rv64_vm_console_instret' LANGUAGE C;

-- Convenience: find the instret when a specific error message was printed
CREATE FUNCTION vm_error_instret(error_pattern TEXT)
RETURNS TABLE(line_num INT, instret BIGINT, line_text TEXT)
LANGUAGE SQL AS $$
    SELECT * FROM vm_console_instret()
    WHERE line_text LIKE '%' || error_pattern || '%';
$$;


-- ============================================================
-- PHASE 0: FULL OBSERVABILITY
-- Every internal state variable visible from SQL.
-- ============================================================

-- ---- Debug State (all instrumentation flags + counters) ----

CREATE TYPE vm_dbg_state_t AS (
    cycle            BIGINT,     -- cpu->cycle (total cycles including WFI)
    instret          BIGINT,     -- cpu->instret (real instructions executed)
    halted           BOOLEAN,    -- cpu->halted (in WFI)
    error            BOOLEAN,    -- cpu->error (fatal)
    run_exit_reason  INT,        -- 0=none 1=budget 2=error 3=debug_halt 4=wfi_timeout
    debug_halt       BOOLEAN,    -- dbg.debug_halt (our instrumentation stopped VM)
    break_active     BOOLEAN,    -- breakpoint armed?
    break_pc         BIGINT,     -- breakpoint target PC
    break_halt       BOOLEAN,    -- halt-on-breakpoint mode?
    break_log_count  INT,        -- total breakpoint hits
    watch_active     BOOLEAN,    -- watchpoint armed?
    watch_addr       BIGINT,     -- watchpoint target address
    store_log_count  INT,        -- total watchpoint hits
    console_log_count INT,       -- console lines logged
    -- Trap and FPU state
    satp             BIGINT,     -- page table root
    fcsr             INT,        -- FP control/status register
    mepc             BIGINT,     -- M-mode exception PC
    mcause           BIGINT,     -- M-mode trap cause
    sepc             BIGINT,     -- S-mode exception PC
    scause           BIGINT      -- S-mode trap cause
);

CREATE FUNCTION vm_dbg_state()
RETURNS vm_dbg_state_t
AS 'linuxsql_vm', 'rv64_vm_dbg_state' LANGUAGE C STRICT;


-- ---- Timer State (CLINT + Sstc) ----

CREATE TYPE vm_timer_state_t AS (
    mtime      BIGINT,     -- current machine timer value
    mtimecmp   BIGINT,     -- M-mode timer compare (fires when mtime >= mtimecmp)
    stimecmp   BIGINT,     -- S-mode timer compare (Sstc extension)
    delta_m    BIGINT,     -- mtimecmp - mtime (how far until M-mode timer fires)
    delta_s    BIGINT      -- stimecmp - mtime (how far until S-mode timer fires)
);

CREATE FUNCTION vm_timer_state()
RETURNS vm_timer_state_t
AS 'linuxsql_vm', 'rv64_vm_timer_state' LANGUAGE C STRICT;


-- ---- Run Exit Reason ----

CREATE FUNCTION vm_run_reason()
RETURNS TEXT
AS 'linuxsql_vm', 'rv64_vm_run_reason' LANGUAGE C STRICT;


-- ---- Convenience: full dashboard in one query ----

CREATE FUNCTION vm_dashboard()
RETURNS TABLE(metric TEXT, value TEXT)
LANGUAGE plpgsql AS $$
DECLARE
    d vm_dbg_state_t;
    t vm_timer_state_t;
    s vm_state_t;
    reason TEXT;
    r vm_regs_t;
BEGIN
    SELECT * INTO d FROM vm_dbg_state();
    SELECT * INTO t FROM vm_timer_state();
    SELECT vm_run_reason() INTO reason;
    SELECT * INTO s FROM vm_state();
    SELECT * INTO r FROM vm_regs();

    -- Section: Execution
    metric := '--- EXECUTION ---'; value := ''; RETURN NEXT;
    metric := 'pc';             value := '0x' || lpad(to_hex(r.pc), 16, '0'); RETURN NEXT;
    metric := 'priv';           value := CASE s.priv WHEN 3 THEN 'M' WHEN 1 THEN 'S' WHEN 0 THEN 'U' ELSE s.priv::TEXT END; RETURN NEXT;
    metric := 'cycle';          value := d.cycle::TEXT;          RETURN NEXT;
    metric := 'instret';        value := d.instret::TEXT;        RETURN NEXT;
    metric := 'halted';         value := d.halted::TEXT;         RETURN NEXT;
    metric := 'error';          value := d.error::TEXT;          RETURN NEXT;
    metric := 'run_exit_reason'; value := reason;                RETURN NEXT;

    -- Section: Debug Instrumentation
    metric := '--- DEBUG ---';  value := ''; RETURN NEXT;
    metric := 'debug_halt';     value := d.debug_halt::TEXT;     RETURN NEXT;
    metric := 'break_active';   value := d.break_active::TEXT;   RETURN NEXT;
    metric := 'break_pc';       value := '0x' || lpad(to_hex(d.break_pc), 16, '0'); RETURN NEXT;
    metric := 'break_halt';     value := d.break_halt::TEXT;     RETURN NEXT;
    metric := 'break_hits';     value := d.break_log_count::TEXT; RETURN NEXT;
    metric := 'watch_active';   value := d.watch_active::TEXT;   RETURN NEXT;
    metric := 'watch_addr';     value := '0x' || lpad(to_hex(d.watch_addr), 16, '0'); RETURN NEXT;
    metric := 'watch_hits';     value := d.store_log_count::TEXT; RETURN NEXT;
    metric := 'console_lines';  value := d.console_log_count::TEXT; RETURN NEXT;

    -- Section: Timers
    metric := '--- TIMERS ---'; value := ''; RETURN NEXT;
    metric := 'mtime';          value := t.mtime::TEXT;          RETURN NEXT;
    metric := 'mtimecmp';       value := t.mtimecmp::TEXT;       RETURN NEXT;
    metric := 'stimecmp';       value := t.stimecmp::TEXT;       RETURN NEXT;
    metric := 'delta_m';        value := t.delta_m::TEXT;        RETURN NEXT;
    metric := 'delta_s';        value := t.delta_s::TEXT;        RETURN NEXT;

    -- Section: Trap State
    metric := '--- TRAPS ---';  value := ''; RETURN NEXT;
    metric := 'mstatus';        value := '0x' || lpad(to_hex(s.mstatus), 16, '0'); RETURN NEXT;
    metric := 'mip';            value := '0x' || lpad(to_hex(s.mip), 16, '0'); RETURN NEXT;
    metric := 'mie';            value := '0x' || lpad(to_hex(s.mie), 16, '0'); RETURN NEXT;
    metric := 'mideleg';        value := '0x' || lpad(to_hex(s.mideleg), 16, '0'); RETURN NEXT;

    -- Section: PLIC
    metric := '--- PLIC ---';   value := ''; RETURN NEXT;
    metric := 'plic_pending';   value := s.plic_pending::TEXT;   RETURN NEXT;
    metric := 'plic_enable_s';  value := s.plic_enable_s::TEXT;  RETURN NEXT;
    metric := 'plic_claim_s';   value := s.plic_claim_s::TEXT;   RETURN NEXT;

    -- Section: Console
    metric := '--- CONSOLE ---'; value := ''; RETURN NEXT;
    metric := 'console_bytes';  value := s.console_bytes::TEXT;  RETURN NEXT;

    -- Section: MMU / FPU
    metric := '--- MMU/FPU ---'; value := ''; RETURN NEXT;
    metric := 'satp';           value := '0x' || lpad(to_hex(d.satp), 16, '0'); RETURN NEXT;
    metric := 'fcsr';           value := '0x' || lpad(to_hex(d.fcsr), 8, '0'); RETURN NEXT;
    metric := 'mepc';           value := '0x' || lpad(to_hex(d.mepc), 16, '0'); RETURN NEXT;
    metric := 'mcause';         value := '0x' || lpad(to_hex(d.mcause), 16, '0'); RETURN NEXT;
    metric := 'sepc';           value := '0x' || lpad(to_hex(d.sepc), 16, '0'); RETURN NEXT;
    metric := 'scause';         value := '0x' || lpad(to_hex(d.scause), 16, '0'); RETURN NEXT;
END;
$$;


-- ============================================================
-- SNAPSHOTS — save/restore/inspect VM state across sessions.
-- ============================================================

CREATE FUNCTION vm_save(path TEXT)
RETURNS TEXT
AS 'linuxsql_vm', 'rv64_vm_save' LANGUAGE C;

CREATE FUNCTION vm_load(path TEXT)
RETURNS TEXT
AS 'linuxsql_vm', 'rv64_vm_load' LANGUAGE C;

CREATE TYPE vm_snapshot_info_t AS (
    pc               BIGINT,
    priv             INT,
    instret          BIGINT,
    cycle            BIGINT,
    mtime            BIGINT,
    halted           BOOLEAN,
    error            BOOLEAN,
    run_exit_reason  INT,
    satp             BIGINT,
    mstatus          BIGINT,
    ram_mb           INT,
    console_bytes    INT
);

CREATE FUNCTION vm_snapshot_info(path TEXT)
RETURNS vm_snapshot_info_t
AS 'linuxsql_vm', 'rv64_vm_snapshot_info' LANGUAGE C;

-- Convenience: human-readable snapshot summary
CREATE FUNCTION vm_snapshot_summary(path TEXT)
RETURNS TABLE(metric TEXT, value TEXT)
LANGUAGE plpgsql AS $$
DECLARE
    s vm_snapshot_info_t;
BEGIN
    SELECT * INTO s FROM vm_snapshot_info(path);
    metric := 'pc';       value := '0x' || lpad(to_hex(s.pc), 16, '0'); RETURN NEXT;
    metric := 'priv';     value := CASE s.priv WHEN 3 THEN 'M' WHEN 1 THEN 'S' WHEN 0 THEN 'U' ELSE s.priv::TEXT END; RETURN NEXT;
    metric := 'instret';  value := s.instret::TEXT; RETURN NEXT;
    metric := 'cycle';    value := s.cycle::TEXT; RETURN NEXT;
    metric := 'mtime';    value := s.mtime::TEXT; RETURN NEXT;
    metric := 'halted';   value := s.halted::TEXT; RETURN NEXT;
    metric := 'error';    value := s.error::TEXT; RETURN NEXT;
    metric := 'ram_mb';   value := s.ram_mb::TEXT; RETURN NEXT;
    metric := 'console';  value := s.console_bytes || ' bytes'; RETURN NEXT;
END;
$$;


-- ============================================================
-- PERSISTENCE — VM state survives connection close.
--
-- The VM runs in process-local memory for speed, but is
-- automatically saved to PostgreSQL large objects when the
-- backend disconnects. On next vm_boot(), saved state is
-- restored instead of cold-booting.
-- ============================================================

CREATE TABLE vm_state (
    id         INTEGER PRIMARY KEY DEFAULT 1 CHECK (id = 1),  -- singleton
    cpu_state  BYTEA,            -- serialized struct rv64_cpu
    console    BYTEA,            -- console ring buffer
    ram_oid    OID,              -- pg_largeobject: guest RAM
    disk_oid   OID,              -- pg_largeobject: virtio disk
    ram_size   BIGINT,
    disk_size  BIGINT,
    instret    BIGINT,
    pc         BIGINT,
    cycle      BIGINT,
    vm_dsm_handle BIGINT,
    vm_worker_pid INTEGER,
    saved_at   TIMESTAMPTZ DEFAULT now()
);

-- Delete saved VM state, forcing a cold boot on next vm_boot().
CREATE FUNCTION vm_reset()
RETURNS TEXT
AS 'linuxsql_vm', 'rv64_vm_reset' LANGUAGE C;

-- Halt the running execution synchronously and unmap DSM gracefully without DB deletion
CREATE FUNCTION vm_halt()
RETURNS TEXT
AS 'linuxsql_vm', 'rv64_vm_halt' LANGUAGE C;


-- ============================================================
-- INTROSPECTION — FP registers, memory write, disassembly.
-- ============================================================

-- ---- Floating-Point Registers ----

CREATE FUNCTION vm_fregs()
RETURNS TABLE(reg INT, name TEXT, hex TEXT, as_float FLOAT4, as_double FLOAT8)
AS 'linuxsql_vm', 'rv64_vm_fregs' LANGUAGE C;


-- (vm_poke already defined above)


-- ---- Disassembler ----

CREATE FUNCTION vm_disasm(addr BIGINT, count INT DEFAULT 20)
RETURNS TABLE(addr TEXT, hex TEXT, insn TEXT)
AS 'linuxsql_vm', 'rv64_vm_disasm' LANGUAGE C;

-- Convenience: disassemble around current PC
CREATE FUNCTION vm_disasm_at_pc(before_count INT DEFAULT 5, after_count INT DEFAULT 15)
RETURNS TABLE(marker TEXT, addr TEXT, hex TEXT, insn TEXT)
LANGUAGE plpgsql AS $$
DECLARE
    cur_pc BIGINT;
    r RECORD;
    total INT;
    start_addr BIGINT;
BEGIN
    SELECT pc INTO cur_pc FROM vm_regs();
    total := before_count + 1 + after_count;
    -- Approximate: assume 4 bytes per instruction for backward scan
    start_addr := cur_pc - (before_count * 4);

    FOR r IN SELECT d.addr, d.hex, d.insn FROM vm_disasm(start_addr, total) d
    LOOP
        IF r.addr = '0x' || lpad(to_hex(cur_pc), 1, '') OR
           r.addr = '0x' || to_hex(cur_pc) THEN
            marker := '>>>';
        ELSE
            marker := '   ';
        END IF;
        addr := r.addr;
        hex := r.hex;
        insn := r.insn;
        RETURN NEXT;
    END LOOP;
END;
$$;


-- ============================================================
-- MMU INTROSPECTION — page table walk, physical memory, TLB.
-- ============================================================

-- ---- Page Table Walk ----

CREATE TYPE vm_translate_t AS (
    mode            INT,        -- satp mode (0=bare, 8=Sv39)
    satp_ppn        BIGINT,     -- root page table PPN
    vpn2            INT,        -- VPN[2] (9 bits)
    vpn1            INT,        -- VPN[1] (9 bits)
    vpn0            INT,        -- VPN[0] (9 bits)
    page_offset     INT,        -- 12-bit page offset
    pte2_addr       BIGINT,     -- level 2 PTE physical address
    pte2_val        BIGINT,     -- level 2 PTE value
    pte2_flags      TEXT,       -- level 2 PTE flags (VRWXUGAD)
    pte1_addr       BIGINT,     -- level 1 PTE physical address
    pte1_val        BIGINT,     -- level 1 PTE value
    pte1_flags      TEXT,       -- level 1 PTE flags
    pte0_addr       BIGINT,     -- level 0 PTE physical address
    pte0_val        BIGINT,     -- level 0 PTE value
    pte0_flags      TEXT,       -- level 0 PTE flags
    leaf_level      INT,        -- which level was the leaf (-1=bare/M)
    phys_addr       BIGINT,     -- translated physical address
    fault_reason    TEXT        -- NULL if OK, else reason
);

CREATE FUNCTION vm_translate(vaddr BIGINT)
RETURNS vm_translate_t
AS 'linuxsql_vm', 'rv64_vm_translate' LANGUAGE C;


-- ---- Physical Memory Access ----

CREATE FUNCTION vm_physpeek(paddr BIGINT, size INT DEFAULT 8)
RETURNS BIGINT
AS 'linuxsql_vm', 'rv64_vm_physpeek' LANGUAGE C;

CREATE FUNCTION vm_physpeek_hex(paddr BIGINT, nbytes INT DEFAULT 16)
RETURNS TEXT
AS 'linuxsql_vm', 'rv64_vm_physpeek_hex' LANGUAGE C;


-- ---- TLB Dump ----

CREATE FUNCTION vm_tlb_dump()
RETURNS TABLE(idx INT, vpn TEXT, ppn TEXT, perm TEXT, level INT, dirty BOOLEAN, host_base TEXT, host_valid BOOLEAN)
AS 'linuxsql_vm', 'rv64_vm_tlb_dump' LANGUAGE C;

-- Convenience: show only TLB entries with invalid host_base (corruption candidates)
CREATE VIEW vm_tlb_bad AS
SELECT * FROM vm_tlb_dump() WHERE NOT host_valid;


-- ============================================================
-- TRAP/CSR INTROSPECTION — decoded cause, delegation, full state.
-- ============================================================

-- ---- Raw CSR access ----

CREATE TYPE vm_csrs_t AS (
    mstatus     BIGINT,
    medeleg     BIGINT,
    mideleg     BIGINT,
    mie         BIGINT,
    mip         BIGINT,
    mtvec       BIGINT,
    mepc        BIGINT,
    mcause      BIGINT,
    mtval       BIGINT,
    stvec       BIGINT,
    sepc        BIGINT,
    scause      BIGINT,
    stval       BIGINT,
    satp        BIGINT,
    priv        INT
);

CREATE FUNCTION vm_csrs()
RETURNS vm_csrs_t
AS 'linuxsql_vm', 'rv64_vm_csrs' LANGUAGE C;


-- ---- Cause decoder ----

CREATE FUNCTION vm_decode_cause(cause BIGINT)
RETURNS TEXT
LANGUAGE plpgsql IMMUTABLE AS $$
DECLARE
    is_interrupt BOOLEAN;
    code INT;
BEGIN
    -- Bit 63 distinguishes interrupts from exceptions
    is_interrupt := (cause < 0);  -- signed: bit 63 set means negative
    code := (cause & x'7FFFFFFFFFFFFFFF'::bigint)::int;

    IF is_interrupt THEN
        RETURN CASE code
            WHEN 1  THEN 'S_SOFTWARE'
            WHEN 3  THEN 'M_SOFTWARE'
            WHEN 5  THEN 'S_TIMER'
            WHEN 7  THEN 'M_TIMER'
            WHEN 9  THEN 'S_EXTERNAL'
            WHEN 11 THEN 'M_EXTERNAL'
            ELSE 'IRQ_' || code
        END;
    ELSE
        RETURN CASE code
            WHEN 0  THEN 'INSN_MISALIGN'
            WHEN 1  THEN 'INSN_ACCESS'
            WHEN 2  THEN 'ILLEGAL_INSN'
            WHEN 3  THEN 'BREAKPOINT'
            WHEN 4  THEN 'LOAD_MISALIGN'
            WHEN 5  THEN 'LOAD_ACCESS'
            WHEN 6  THEN 'STORE_MISALIGN'
            WHEN 7  THEN 'STORE_ACCESS'
            WHEN 8  THEN 'ECALL_U'
            WHEN 9  THEN 'ECALL_S'
            WHEN 11 THEN 'ECALL_M'
            WHEN 12 THEN 'INSN_PAGE_FAULT'
            WHEN 13 THEN 'LOAD_PAGE_FAULT'
            WHEN 15 THEN 'STORE_PAGE_FAULT'
            ELSE 'EXC_' || code
        END;
    END IF;
END;
$$;


-- ---- Delegation decoder ----

CREATE FUNCTION vm_decode_deleg(deleg BIGINT, is_interrupt BOOLEAN DEFAULT false)
RETURNS TABLE(bitnum INT, name TEXT, delegated BOOLEAN)
LANGUAGE plpgsql IMMUTABLE AS $$
BEGIN
    FOR bitnum IN 0..19 LOOP
        delegated := ((deleg >> bitnum) & 1) = 1;
        IF is_interrupt THEN
            name := CASE bitnum
                WHEN 1  THEN 'S_SOFTWARE'
                WHEN 3  THEN 'M_SOFTWARE'
                WHEN 5  THEN 'S_TIMER'
                WHEN 7  THEN 'M_TIMER'
                WHEN 9  THEN 'S_EXTERNAL'
                WHEN 11 THEN 'M_EXTERNAL'
                ELSE 'IRQ_' || bitnum
            END;
        ELSE
            name := CASE bitnum
                WHEN 0  THEN 'INSN_MISALIGN'
                WHEN 1  THEN 'INSN_ACCESS'
                WHEN 2  THEN 'ILLEGAL_INSN'
                WHEN 3  THEN 'BREAKPOINT'
                WHEN 4  THEN 'LOAD_MISALIGN'
                WHEN 5  THEN 'LOAD_ACCESS'
                WHEN 6  THEN 'STORE_MISALIGN'
                WHEN 7  THEN 'STORE_ACCESS'
                WHEN 8  THEN 'ECALL_U'
                WHEN 9  THEN 'ECALL_S'
                WHEN 11 THEN 'ECALL_M'
                WHEN 12 THEN 'INSN_PAGE_FAULT'
                WHEN 13 THEN 'LOAD_PAGE_FAULT'
                WHEN 15 THEN 'STORE_PAGE_FAULT'
                ELSE 'EXC_' || bitnum
            END;
        END IF;
        -- Only return bits that are actually set
        IF delegated THEN RETURN NEXT; END IF;
    END LOOP;
END;
$$;


-- ---- mstatus decoder ----

CREATE FUNCTION vm_decode_mstatus(mstatus BIGINT)
RETURNS TABLE(field TEXT, value TEXT)
LANGUAGE plpgsql IMMUTABLE AS $$
BEGIN
    field := 'SIE';   value := CASE WHEN (mstatus >> 1) & 1 = 1 THEN 'enabled' ELSE 'disabled' END; RETURN NEXT;
    field := 'MIE';   value := CASE WHEN (mstatus >> 3) & 1 = 1 THEN 'enabled' ELSE 'disabled' END; RETURN NEXT;
    field := 'SPIE';  value := ((mstatus >> 5) & 1)::text;  RETURN NEXT;
    field := 'MPIE';  value := ((mstatus >> 7) & 1)::text;  RETURN NEXT;
    field := 'SPP';   value := CASE WHEN (mstatus >> 8) & 1 = 1 THEN 'S' ELSE 'U' END; RETURN NEXT;
    field := 'MPP';   value := CASE (mstatus >> 11) & 3 WHEN 0 THEN 'U' WHEN 1 THEN 'S' WHEN 3 THEN 'M' ELSE '?' END; RETURN NEXT;
    field := 'FS';    value := CASE (mstatus >> 13) & 3 WHEN 0 THEN 'Off' WHEN 1 THEN 'Initial' WHEN 2 THEN 'Clean' WHEN 3 THEN 'Dirty' END; RETURN NEXT;
    field := 'SUM';   value := CASE WHEN (mstatus >> 18) & 1 = 1 THEN 'enabled' ELSE 'disabled' END; RETURN NEXT;
    field := 'MXR';   value := CASE WHEN (mstatus >> 19) & 1 = 1 THEN 'enabled' ELSE 'disabled' END; RETURN NEXT;
    field := 'TVM';   value := CASE WHEN (mstatus >> 20) & 1 = 1 THEN 'trapped' ELSE 'allowed' END; RETURN NEXT;
END;
$$;


-- ---- One-stop trap info view ----

CREATE FUNCTION vm_trap_info()
RETURNS TABLE(section TEXT, field TEXT, value TEXT)
LANGUAGE plpgsql AS $$
DECLARE
    c vm_csrs_t;
    priv_name TEXT;
BEGIN
    SELECT * INTO c FROM vm_csrs();

    priv_name := CASE c.priv WHEN 0 THEN 'U' WHEN 1 THEN 'S' WHEN 3 THEN 'M' ELSE '?' END;

    -- Current mode
    section := 'MODE';    field := 'priv';       value := priv_name; RETURN NEXT;

    -- M-mode trap state
    section := 'M-TRAP';  field := 'mtvec';      value := '0x' || lpad(to_hex(c.mtvec), 16, '0'); RETURN NEXT;
    section := 'M-TRAP';  field := 'mepc';       value := '0x' || lpad(to_hex(c.mepc), 16, '0'); RETURN NEXT;
    section := 'M-TRAP';  field := 'mcause';     value := vm_decode_cause(c.mcause); RETURN NEXT;
    section := 'M-TRAP';  field := 'mcause_raw'; value := '0x' || lpad(to_hex(c.mcause), 16, '0'); RETURN NEXT;
    section := 'M-TRAP';  field := 'mtval';      value := '0x' || lpad(to_hex(c.mtval), 16, '0'); RETURN NEXT;

    -- S-mode trap state
    section := 'S-TRAP';  field := 'stvec';      value := '0x' || lpad(to_hex(c.stvec), 16, '0'); RETURN NEXT;
    section := 'S-TRAP';  field := 'sepc';       value := '0x' || lpad(to_hex(c.sepc), 16, '0'); RETURN NEXT;
    section := 'S-TRAP';  field := 'scause';     value := vm_decode_cause(c.scause); RETURN NEXT;
    section := 'S-TRAP';  field := 'scause_raw'; value := '0x' || lpad(to_hex(c.scause), 16, '0'); RETURN NEXT;
    section := 'S-TRAP';  field := 'stval';      value := '0x' || lpad(to_hex(c.stval), 16, '0'); RETURN NEXT;

    -- Interrupt state
    section := 'IRQ';     field := 'mstatus';    value := '0x' || lpad(to_hex(c.mstatus), 16, '0'); RETURN NEXT;
    section := 'IRQ';     field := 'mie';        value := '0x' || lpad(to_hex(c.mie), 16, '0'); RETURN NEXT;
    section := 'IRQ';     field := 'mip';        value := '0x' || lpad(to_hex(c.mip), 16, '0'); RETURN NEXT;
    section := 'IRQ';     field := 'satp';       value := '0x' || lpad(to_hex(c.satp), 16, '0'); RETURN NEXT;

    -- Delegation
    section := 'DELEG';   field := 'medeleg';    value := '0x' || lpad(to_hex(c.medeleg), 16, '0'); RETURN NEXT;
    section := 'DELEG';   field := 'mideleg';    value := '0x' || lpad(to_hex(c.mideleg), 16, '0'); RETURN NEXT;
END;
$$;

-- Show which exceptions/interrupts are delegated to S-mode
CREATE VIEW vm_delegated AS
SELECT 'exception' AS type, d.* FROM vm_csrs() c, vm_decode_deleg(c.medeleg, false) d
UNION ALL
SELECT 'interrupt' AS type, d.* FROM vm_csrs() c, vm_decode_deleg(c.mideleg, true) d
ORDER BY type, bitnum;


-- ============================================================
-- TRAP LOG — exception ring buffer + syscall decoder.
-- ============================================================

CREATE FUNCTION vm_trap_log()
RETURNS TABLE(seq INT, cause TEXT, tval TEXT, pc TEXT, priv TEXT,
              instret BIGINT, a7 BIGINT, a0 TEXT, a1 TEXT, a2 TEXT,
              ret_a0 TEXT, ret_pc TEXT)
AS 'linuxsql_vm', 'rv64_vm_trap_log' LANGUAGE C;


-- ---- RISC-V Linux syscall name decoder ----

CREATE FUNCTION vm_decode_syscall(num BIGINT)
RETURNS TEXT
LANGUAGE plpgsql IMMUTABLE AS $$
BEGIN
    RETURN CASE num
        WHEN 17  THEN 'getcwd'
        WHEN 23  THEN 'dup'
        WHEN 24  THEN 'dup3'
        WHEN 25  THEN 'fcntl'
        WHEN 29  THEN 'ioctl'
        WHEN 33  THEN 'mknodat'
        WHEN 34  THEN 'mkdirat'
        WHEN 35  THEN 'unlinkat'
        WHEN 37  THEN 'linkat'
        WHEN 38  THEN 'renameat'
        WHEN 46  THEN 'ftruncate'
        WHEN 48  THEN 'faccessat'
        WHEN 49  THEN 'chdir'
        WHEN 53  THEN 'fchmodat'
        WHEN 54  THEN 'fchownat'
        WHEN 55  THEN 'fchown'
        WHEN 56  THEN 'openat'
        WHEN 57  THEN 'close'
        WHEN 59  THEN 'pipe2'
        WHEN 61  THEN 'getdents64'
        WHEN 62  THEN 'lseek'
        WHEN 63  THEN 'read'
        WHEN 64  THEN 'write'
        WHEN 66  THEN 'writev'
        WHEN 67  THEN 'pread64'
        WHEN 68  THEN 'pwrite64'
        WHEN 73  THEN 'ppoll'
        WHEN 78  THEN 'readlinkat'
        WHEN 79  THEN 'newfstatat'
        WHEN 80  THEN 'fstat'
        WHEN 93  THEN 'exit'
        WHEN 94  THEN 'exit_group'
        WHEN 96  THEN 'set_tid_address'
        WHEN 98  THEN 'futex'
        WHEN 99  THEN 'set_robust_list'
        WHEN 101 THEN 'nanosleep'
        WHEN 113 THEN 'clock_gettime'
        WHEN 116 THEN 'syslog'
        WHEN 124 THEN 'sched_yield'
        WHEN 129 THEN 'kill'
        WHEN 130 THEN 'tkill'
        WHEN 131 THEN 'tgkill'
        WHEN 134 THEN 'rt_sigaction'
        WHEN 135 THEN 'rt_sigprocmask'
        WHEN 137 THEN 'rt_sigtimedwait'
        WHEN 139 THEN 'rt_sigreturn'
        WHEN 144 THEN 'setgid'
        WHEN 146 THEN 'setuid'
        WHEN 153 THEN 'times'
        WHEN 155 THEN 'getpgid'
        WHEN 157 THEN 'setsid'
        WHEN 160 THEN 'uname'
        WHEN 161 THEN 'sethostname'
        WHEN 163 THEN 'getrlimit'
        WHEN 164 THEN 'setrlimit'
        WHEN 165 THEN 'getrusage'
        WHEN 166 THEN 'umask'
        WHEN 167 THEN 'prctl'
        WHEN 172 THEN 'getpid'
        WHEN 173 THEN 'getppid'
        WHEN 174 THEN 'getuid'
        WHEN 175 THEN 'geteuid'
        WHEN 176 THEN 'getgid'
        WHEN 177 THEN 'getegid'
        WHEN 178 THEN 'gettid'
        WHEN 179 THEN 'sysinfo'
        WHEN 198 THEN 'socket'
        WHEN 200 THEN 'bind'
        WHEN 201 THEN 'listen'
        WHEN 202 THEN 'accept'
        WHEN 203 THEN 'connect'
        WHEN 204 THEN 'getsockname'
        WHEN 206 THEN 'sendto'
        WHEN 207 THEN 'recvfrom'
        WHEN 208 THEN 'setsockopt'
        WHEN 209 THEN 'getsockopt'
        WHEN 210 THEN 'shutdown'
        WHEN 214 THEN 'brk'
        WHEN 215 THEN 'munmap'
        WHEN 216 THEN 'mremap'
        WHEN 220 THEN 'clone'
        WHEN 221 THEN 'execve'
        WHEN 222 THEN 'mmap'
        WHEN 226 THEN 'mprotect'
        WHEN 233 THEN 'madvise'
        WHEN 260 THEN 'wait4'
        WHEN 261 THEN 'prlimit64'
        WHEN 276 THEN 'renameat2'
        WHEN 278 THEN 'getrandom'
        WHEN 291 THEN 'statx'
        WHEN 435 THEN 'clone3'
        ELSE 'syscall_' || num
    END;
END;
$$;


-- ---- Convenience views ----

-- Syscall trace: only ECALL_U traps with decoded syscall names
CREATE VIEW vm_syscall_trace AS
SELECT seq, vm_decode_syscall(a7) AS syscall, a7 AS num,
       a0, a1, a2, pc, instret
FROM vm_trap_log()
WHERE cause = 'ECALL_U'
ORDER BY seq;

-- Page fault trace: only page fault traps
CREATE VIEW vm_pagefault_trace AS
SELECT seq, cause, tval AS fault_addr, pc, priv, instret
FROM vm_trap_log()
WHERE cause IN ('INSN_PAGE_FAULT', 'LOAD_PAGE_FAULT', 'STORE_PAGE_FAULT')
ORDER BY seq;

-- MMU Hotspots: Memory Thrashing View
-- Aggregates raw page fault strings down to 4KB aligned physical pages
-- to detect thrashing segments and memory densities dynamically.
CREATE VIEW vm_mmu_hotspots AS
WITH mmu_events AS (
    SELECT cause, 
           ('x' || lpad(fault_addr, 16, '0'))::bit(64)::bigint AS addr_int
    FROM vm_pagefault_trace
),
page_boundaries AS (
    SELECT cause,
           (addr_int / 4096) * 4096 AS base_page_int
    FROM mmu_events
)
SELECT '0x' || lpad(to_hex(base_page_int), 8, '0') AS memory_page,
       cause AS fault_type,
       COUNT(*) AS hit_count
FROM page_boundaries
WHERE base_page_int > 0 
GROUP BY base_page_int, cause
ORDER BY hit_count DESC;

-- Last N traps (most recent first)
CREATE VIEW vm_trap_recent AS
SELECT * FROM vm_trap_log() ORDER BY seq DESC LIMIT 50;

-- Signal delivery detection: when the kernel returns to user mode
-- at a different PC than expected, it's delivering a signal.
-- ret_pc = 0 means SRET hasn't happened yet for this entry.
CREATE VIEW vm_signal_trace AS
SELECT seq, cause,
       CASE WHEN cause = 'ECALL_U' THEN vm_decode_syscall(a7) ELSE cause END AS what,
       ret_a0, ret_pc, pc, instret
FROM vm_trap_log()
WHERE ret_pc != '0x0'           -- SRET happened
  AND cause = 'ECALL_U'         -- was a syscall
  AND ret_pc != pc              -- returned to different address (signal)
ORDER BY seq;

-- Syscall return values: shows a0 at entry and ret_a0 at return
CREATE VIEW vm_syscall_returns AS
SELECT seq, vm_decode_syscall(a7) AS syscall, a7 AS num,
       a0 AS arg0, a1 AS arg1, ret_a0, ret_pc,
       pc, instret
FROM vm_trap_log()
WHERE cause = 'ECALL_U' AND ret_pc != '0x0'
ORDER BY seq;


-- ============================================================
-- TEST RUNNER — structured test execution with diagnostics.
-- SQL is the primary troubleshooting interface.
-- ============================================================

/*
 * vm_test_exec(cmd, expected, timeout) → TABLE
 *
 * Run a single command in the VM, check if output matches expected,
 * and if it crashed, auto-capture trap state + registers + disassembly.
 *
 * Usage:
 *   SELECT * FROM vm_test_exec('postgres --version', 'PostgreSQL');
 *   SELECT * FROM vm_test_exec('/usr/local/pgsql/bin/glibc_malloc', 'glibc-malloc-ok');
 */
CREATE FUNCTION vm_test_exec(
    cmd TEXT,
    expected TEXT DEFAULT NULL,
    timeout_cycles BIGINT DEFAULT 10000000000
)
RETURNS TABLE(
    test_name   TEXT,
    passed      BOOLEAN,
    output      TEXT,
    crash_info  TEXT
)
LANGUAGE plpgsql AS $$
DECLARE
    result TEXT;
    is_crash BOOLEAN;
    crash_detail TEXT := NULL;
    trap RECORD;
BEGIN
    test_name := cmd;

    -- Run the command
    result := vm_exec(cmd, timeout_cycles);
    output := result;

    -- Detect crash: kernel crash dumps contain 'Segmentation fault' or 'Illegal instruction'
    is_crash := (result LIKE '%Segmentation fault%')
             OR (result LIKE '%Illegal instruction%')
             OR (result LIKE '%unhandled signal%');

    -- If crashed, auto-capture diagnostic state
    IF is_crash THEN
        passed := false;
        -- Grab recent traps for context
        SELECT string_agg(
            seq || ': ' || cause || ' pc=' || pc || ' tval=' || tval,
            E'\n' ORDER BY seq DESC
        ) INTO crash_detail
        FROM (SELECT * FROM vm_trap_log() ORDER BY seq DESC LIMIT 5) t;
        crash_info := crash_detail;
    ELSIF expected IS NOT NULL THEN
        passed := (result LIKE '%' || expected || '%');
        IF NOT passed THEN
            crash_info := 'expected "' || expected || '" not found in output';
        END IF;
    ELSE
        passed := true;
    END IF;

    RETURN NEXT;
END;
$$;


/*
 * vm_test_suite() → TABLE
 *
 * Run the standard test battery against a booted VM.
 * Assumes the VM is already booted and at the shell prompt.
 */
CREATE FUNCTION vm_test_suite()
RETURNS TABLE(
    tier        TEXT,
    test_name   TEXT,
    passed      BOOLEAN,
    output      TEXT,
    crash_info  TEXT
)
LANGUAGE plpgsql AS $$
DECLARE
    r RECORD;
BEGIN
    -- Tier 1: Static binary tests
    tier := 'static';
    FOR r IN SELECT * FROM vm_test_exec('/usr/local/pgsql/bin/glibc_write', 'glibc-write-ok') LOOP
        test_name := r.test_name; passed := r.passed; output := r.output; crash_info := r.crash_info; RETURN NEXT;
    END LOOP;

    FOR r IN SELECT * FROM vm_test_exec('/usr/local/pgsql/bin/glibc_printf', 'glibc-printf-ok') LOOP
        test_name := r.test_name; passed := r.passed; output := r.output; crash_info := r.crash_info; RETURN NEXT;
    END LOOP;

    FOR r IN SELECT * FROM vm_test_exec('/usr/local/pgsql/bin/glibc_malloc', 'glibc-malloc-ok') LOOP
        test_name := r.test_name; passed := r.passed; output := r.output; crash_info := r.crash_info; RETURN NEXT;
    END LOOP;

    FOR r IN SELECT * FROM vm_test_exec('/usr/local/pgsql/bin/test_static', 'static-ok') LOOP
        test_name := r.test_name; passed := r.passed; output := r.output; crash_info := r.crash_info; RETURN NEXT;
    END LOOP;

    -- Tier 2: Postgres binary tests
    tier := 'postgres';
    FOR r IN SELECT * FROM vm_test_exec('/usr/local/pgsql/bin/postgres --version', 'PostgreSQL') LOOP
        test_name := r.test_name; passed := r.passed; output := r.output; crash_info := r.crash_info; RETURN NEXT;
    END LOOP;

    FOR r IN SELECT * FROM vm_test_exec('/usr/local/pgsql/bin/initdb --version', 'PostgreSQL') LOOP
        test_name := r.test_name; passed := r.passed; output := r.output; crash_info := r.crash_info; RETURN NEXT;
    END LOOP;

    -- Tier 3: Dynamic linking tests (the current blocker)
    tier := 'dynamic';
    FOR r IN SELECT * FROM vm_test_exec('/usr/local/pgsql/bin/dynlink_test 2>&1', 'dynlink-ok') LOOP
        test_name := r.test_name; passed := r.passed; output := r.output; crash_info := r.crash_info; RETURN NEXT;
    END LOOP;

    FOR r IN SELECT * FROM vm_test_exec('ls / 2>&1') LOOP
        test_name := r.test_name; passed := r.passed; output := r.output; crash_info := r.crash_info; RETURN NEXT;
    END LOOP;

    -- Tier 4: Shell operations
    tier := 'shell';
    FOR r IN SELECT * FROM vm_test_exec('echo hello_test', 'hello_test') LOOP
        test_name := r.test_name; passed := r.passed; output := r.output; crash_info := r.crash_info; RETURN NEXT;
    END LOOP;

    FOR r IN SELECT * FROM vm_test_exec('echo test_append >> /tmp/test_file && cat /tmp/test_file && rm /tmp/test_file', 'test_append') LOOP
        test_name := r.test_name; passed := r.passed; output := r.output; crash_info := r.crash_info; RETURN NEXT;
    END LOOP;
END;
$$;


/*
 * vm_crash_report() → TABLE
 *
 * Post-mortem after a crash. Captures everything needed to
 * diagnose the failure: registers, recent traps, page faults,
 * disassembly at crash PC, and page table walk of the faulting address.
 */
CREATE FUNCTION vm_crash_report()
RETURNS TABLE(section TEXT, detail TEXT)
LANGUAGE plpgsql AS $$
DECLARE
    r vm_regs_t;
    d vm_dbg_state_t;
    trap RECORD;
    disasm RECORD;
    xlat vm_translate_t;
BEGIN
    SELECT * INTO r FROM vm_regs();
    SELECT * INTO d FROM vm_dbg_state();

    -- Section: State
    section := 'STATE';
    detail := 'pc=0x' || lpad(to_hex(r.pc), 16, '0')
        || ' instret=' || r.instret
        || ' halted=' || d.halted
        || ' error=' || d.error;
    RETURN NEXT;

    -- Section: Last cause
    section := 'CAUSE';
    detail := 'scause=' || vm_decode_cause(d.scause)
        || ' sepc=0x' || lpad(to_hex(d.sepc), 16, '0');
    RETURN NEXT;

    -- Section: Registers
    section := 'REGS';
    detail := r.x_hex;
    RETURN NEXT;

    -- Section: Recent traps
    FOR trap IN SELECT * FROM vm_trap_log() ORDER BY seq DESC LIMIT 10 LOOP
        section := 'TRAP';
        detail := '#' || trap.seq || ' ' || trap.cause
            || ' pc=' || trap.pc || ' tval=' || trap.tval
            || CASE WHEN trap.cause = 'ECALL_U'
                    THEN ' ' || vm_decode_syscall(trap.a7)
                    ELSE '' END;
        RETURN NEXT;
    END LOOP;

    -- Section: Recent page faults
    FOR trap IN SELECT * FROM vm_pagefault_trace LIMIT 5 LOOP
        section := 'PGFAULT';
        detail := '#' || trap.seq || ' ' || trap.cause
            || ' addr=' || trap.fault_addr
            || ' pc=' || trap.pc
            || ' priv=' || trap.priv;
        RETURN NEXT;
    END LOOP;

    -- Section: Disassembly at PC
    FOR disasm IN SELECT * FROM vm_disasm(r.pc - 8, 10) LOOP
        section := 'DISASM';
        detail := disasm.addr || ' ' || disasm.hex || ' ' || disasm.insn;
        RETURN NEXT;
    END LOOP;

    -- Section: Page table walk of PC
    BEGIN
        SELECT * INTO xlat FROM vm_translate(r.pc);
        section := 'XLAT_PC';
        detail := 'vaddr=0x' || lpad(to_hex(r.pc), 16, '0')
            || ' → phys=0x' || lpad(to_hex(xlat.phys_addr), 16, '0')
            || ' leaf_level=' || xlat.leaf_level
            || coalesce(' fault=' || xlat.fault_reason, '');
        RETURN NEXT;
    EXCEPTION WHEN OTHERS THEN
        section := 'XLAT_PC';
        detail := 'translation failed: ' || SQLERRM;
        RETURN NEXT;
    END;
END;
$$;


-- ============================================================
-- JIT DIAGNOSTIC TOOLS — isolate JIT compiler bugs via SQL.
--
-- Three tools for debugging JIT miscompilation:
--   1. vm_jit_block_size(n)    — control compilation granularity
--   2. vm_jit_disable_op(op)   — bisect by opcode class
--   3. vm_jit_compare(n)       — side-by-side interpreter vs JIT
-- ============================================================

-- ---- Tool 1: Block Size Control ----
-- Set max instructions per JIT-compiled block.
-- n=1: single-instruction blocks (tests codegen in isolation)
-- n=0: reset to compile-time default (64)

CREATE FUNCTION vm_jit_block_size(n INT)
RETURNS TEXT
AS 'linuxsql_vm', 'rv64_vm_jit_block_size' LANGUAGE C;

-- ---- Tool 2: Per-Opcode JIT Disable ----
-- Force specific RISC-V opcodes to the interpreter path.
-- Use to bisect which instruction class the JIT miscompiles.
--
-- Common opcodes:
--   0x03 LOAD, 0x23 STORE, 0x13 OP-IMM, 0x33 OP,
--   0x1B OP-IMM-32, 0x3B OP-32, 0x37 LUI, 0x17 AUIPC,
--   0x6F JAL, 0x67 JALR, 0x63 BRANCH

CREATE FUNCTION vm_jit_disable_op(opcode INT)
RETURNS TEXT
AS 'linuxsql_vm', 'rv64_vm_jit_disable_op' LANGUAGE C;

CREATE FUNCTION vm_jit_enable_op(opcode INT)
RETURNS TEXT
AS 'linuxsql_vm', 'rv64_vm_jit_enable_op' LANGUAGE C;

CREATE FUNCTION vm_jit_disabled_ops()
RETURNS TEXT
AS 'linuxsql_vm', 'rv64_vm_jit_disabled_ops' LANGUAGE C;

-- ---- Tool 3: Side-by-Side Comparison ----
-- Run up to n instructions, comparing interpreter vs JIT output
-- for each one. Returns all register divergences found at the
-- first mismatched instruction (empty = no bug in those n insns).
--
-- Usage:
--   SELECT * FROM vm_jit_compare(500000);

CREATE FUNCTION vm_jit_compare(n INT, block_size INT DEFAULT 1)
RETURNS TABLE(insn_num BIGINT, pc BIGINT, hex_insn TEXT,
              reg_name TEXT, interp_val TEXT, jit_val TEXT)
AS 'linuxsql_vm', 'rv64_vm_jit_compare' LANGUAGE C;

-- ---- Convenience: automated opcode bisect ----
-- Tests each JIT-compiled opcode class by disabling it one at a
-- time and checking if OpenSBI boots past the known failure point.
-- Returns which opcode(s) fix the boot when disabled.

CREATE FUNCTION vm_jit_bisect_ops(root_dir TEXT, step_budget INT DEFAULT 500000)
RETURNS TABLE(opcode INT, opcode_name TEXT, boot_fixed BOOLEAN, cbytes INT)
LANGUAGE plpgsql AS $$
DECLARE
    ops INT[] := ARRAY[
        x'03'::int, x'13'::int, x'17'::int, x'1B'::int,
        x'23'::int, x'33'::int, x'37'::int, x'3B'::int,
        x'63'::int, x'67'::int, x'6F'::int
    ];
    op_names TEXT[] := ARRAY[
        'LOAD', 'OP-IMM', 'AUIPC', 'OP-IMM-32',
        'STORE', 'OP', 'LUI', 'OP-32',
        'BRANCH', 'JALR', 'JAL'
    ];
    i INT;
    d RECORD;
BEGIN
    FOR i IN 1..array_length(ops, 1) LOOP
        opcode := ops[i];
        opcode_name := op_names[i];

        -- Cold-boot fresh FIRST
        PERFORM vm_reset();
        PERFORM vm_asset_load('firmware', root_dir || '/vm/fw_jump.bin');
        PERFORM vm_asset_load('kernel',   root_dir || '/vm/kernel.bin');
        PERFORM vm_asset_load('dtb',      root_dir || '/vm/linuxsql.dtb');
        PERFORM vm_asset_load('initrd',   root_dir || '/vm/initramfs.cpio.gz');
        PERFORM vm_asset_load('disk',     root_dir || '/vm/rootfs.img');
        PERFORM vm_boot();

        -- Reset: re-enable all ops
        PERFORM vm_jit_enable_op(o) FROM unnest(ops) AS o;

        -- Disable just this one opcode class
        PERFORM vm_jit_disable_op(opcode);

        -- Step
        PERFORM vm_step(step_budget);

        -- Check if we got console output (OpenSBI banner = success)
        SELECT value::int INTO cbytes
        FROM vm_dashboard() WHERE metric = 'console_bytes';
        cbytes := COALESCE(cbytes, 0);

        boot_fixed := (cbytes > 0);
        RETURN NEXT;
    END LOOP;

    -- Restore: re-enable everything
    PERFORM vm_jit_enable_op(o) FROM unnest(ops) AS o;
END;
$$;
CREATE OR REPLACE FUNCTION vm_inst_log() RETURNS text AS 'linuxsql_vm', 'rv64_vm_inst_log' LANGUAGE C STRICT PARALLEL SAFE;
