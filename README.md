# LinuxSQL: A RISC-V Linux VM Inside PostgreSQL

This project embeds an entire 64-bit RISC-V emulator into a PostgreSQL extension, enabling the execution of an unmodified Linux 6.1 kernel directly within a PostgreSQL background worker. 

The guest's physical memory is mapped to PostgreSQL Large Objects, allowing the 8GB root filesystem to be stored natively within the database cache. Execution is accelerated by a custom `SLJIT` backend that transpiles RISC-V opcodes to native Apple Silicon ARM64 instructions at runtime.

## Introspection Interface

All guest introspection is exposed natively as relational data. The PostgreSQL frontend communicates with the VM background worker over shared memory to expose hardware telemetry, MMU page faults, and kernel data structures directly securely within the SQL interface.

The VM is initialized by calling:
```sql
SELECT * FROM vm_step_until('init:proc', 5000000000);
```

### The Live Process Tree
The guest's Linux process list can be queried using standard `SELECT` statements.

```sql
SELECT tree_display AS "Process", ppid AS "Parent", vmrss_kb || ' KB' AS "RAM"
FROM vm_proc_tree();
```

```text
       Process        | Parent |  RAM
----------------------+--------+---------
 init                 |      0 | 812 KB
 └─ (kthreadd)        |      1 | 0 KB
   └─ (rcu_gp)        |      2 | 0 KB
   └─ (kworker/0:0)   |      2 | 0 KB
 ├─ /bin/login_sh     |      1 | 420 KB
 └─ postgres_agent    |      1 | 2048 KB
```

### Raw MMU Tracing
Segmentation faults and memory access violations inside the guest can be monitored in real-time by selecting from the `vm_pagefault_trace` view.

```sql
SELECT cause AS "Fault Type",
       '0x' || to_hex(fault_addr::bigint) AS "Address",
       '0x' || to_hex(pc::bigint) AS "Program Counter"
FROM vm_pagefault_trace
ORDER BY seq DESC LIMIT 3;
```

```text
        Fault Type        |  Address   | Program Counter
--------------------------+------------+------------------
 Store page fault         | 0x3F00108A | 0xFFFFFFFF8012B4
 Load access fault        | 0x00000000 | 0xFFFFFFFF803AC1
 Instruction page fault   | 0x7FFFF000 | 0x7FFFF000
```

### Hardware Telemetry
System telemetry metrics are mapped to a static SQL function.

```sql
SELECT metric, value FROM vm_dashboard();
```

```text
      metric      |   value
------------------+------------
 hz               | 512000000
 stat_jit_insns   | 28591000
 console_bytes    | 1204
 cpu_temp         | 42
```

### Physical Memory Heatmaps
Guest kernel behavior can be analyzed directly via SQL groupings. The `vm_mmu_hotspots` view aggregates 4KB physical page boundaries dynamically based on raw MMU page faults to identify memory thrashing.

```sql
SELECT memory_page AS "Physical Boundary",
       fault_type AS "Page Fault Class",
       hit_count AS "Trap Frequency"
FROM vm_mmu_hotspots LIMIT 3;
```

```text
 Physical Boundary |   Page Fault Class   | Trap Frequency 
-------------------+----------------------+----------------
 0x3f001000        | LOAD_PAGE_FAULT      |            142
 0x489b0000        | STORE_PAGE_FAULT     |             38
```

## Deep Inception (Nested Virtualization)
The RISC-V SLJIT transpiler correctly implements `FENCE.I` cache coherency and maps memory strictly to PostgreSQL objects. This architecture allows the guest OS to achieve deep nested virtualization natively.

The included `inception_l3.sql` script leverages the guest's 8GB block-device snapshot to initialize three layers of hardware traversal:
**`Host (Apple Silicon) -> VM 1 (PostgreSQL) -> VM 2 (PostgreSQL) -> VM 3 (PostgreSQL)`**

```bash
# Note: Execution time scales exponentially with virtualization depth.
psql -d linuxsql -f inception_l3.sql
```

## Build Instructions
Execute the following commands to initialize the environment:

```bash
make -f Makefile.vm clean reload
psql -d linuxsql -f demo.sql
```

This sequence deploys the PostgreSQL extension, seeds the database with the required firmware binaries, and initializes the Virtual Machine hardware assets.
