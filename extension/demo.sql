-- Boot the Virtual Machine completely within a PostgreSQL transaction
SELECT vm_asset_load('firmware', '/path/to/linuxsql/vm/fw_jump.bin');
SELECT vm_asset_load('kernel', '/path/to/linuxsql/vm/kernel.bin');
SELECT vm_asset_load('dtb', '/path/to/linuxsql/vm/linuxsql.dtb');
SELECT vm_asset_load('initrd', '/path/to/linuxsql/vm/initramfs.cpio.gz');
SELECT vm_asset_load('disk', '/path/to/linuxsql/vm/rootfs.img');
SELECT vm_boot();

-- Let the JIT compile and boot Linux! (Native JIT execution)
SELECT * FROM vm_step_until('init:proc', 5000000000);

-- Give userspace processes some time to spawn
SELECT vm_step(50000000);

-- 1. Inspect the live Linux process tree using purely relational queries
SELECT '\n*** LIVE GUEST OS PROCESS TREE ***' AS log;
SELECT tree_display AS "Process Name", pid AS "PID", ppid AS "Parent",
       state AS "Status", vmrss_kb || ' KB' AS "Memory Usage"
FROM vm_proc_tree();

-- 2. Watch real-time page faults emitted directly from the MMU
SELECT '\n*** MMU PAGE FAULT MATRIX ***' AS log;
SELECT seq AS "Sequence", cause AS "Fault Type",
       '0x' || to_hex(fault_addr::bigint) AS "Memory Address",
       '0x' || to_hex(pc::bigint) AS "Program Counter"
FROM vm_pagefault_trace
ORDER BY seq DESC LIMIT 5;

-- 3. Monitor active hardware telemetry mapped to regular Postgres Tables
SELECT '\n*** REAL-TIME HARDWARE TELEMETRY ***' AS log;
SELECT metric AS "Sensor", value AS "Value"
FROM vm_dashboard()
WHERE metric LIKE 'vda_%' OR metric = 'stat_jit_insns' OR metric = 'console_bytes';

-- 4. Map Memory Thrashing Hotspots (Cross-Subsystem Aggregation)
SELECT '\n*** MMU KERNEL MEMORY HEATMAP ***' AS log;
SELECT memory_page AS "Physical Boundary",
       fault_type AS "Page Fault Class",
       hit_count AS "Trap Frequency"
FROM vm_mmu_hotspots
LIMIT 5;
