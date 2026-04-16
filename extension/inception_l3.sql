\c linuxsql
DROP EXTENSION IF EXISTS linuxsql_vm CASCADE;
CREATE EXTENSION linuxsql_vm;

-- Step 1: Boot Outer Environment (Host)
SELECT vm_asset_load('firmware', :'root' || '/vm/fw_jump.bin');
SELECT vm_asset_load('kernel', :'root' || '/vm/kernel.bin');
SELECT vm_asset_load('dtb', :'root' || '/vm/linuxsql.dtb');
SELECT vm_asset_load('initrd', :'root' || '/vm/initramfs.cpio.gz');
SELECT vm_asset_load('disk', :'root' || '/vm/rootfs.img');
SELECT vm_boot();

DO $$
BEGIN
    RAISE NOTICE 'Booting Layer 1 (VM1)...';
    PERFORM vm_step_until('database system is ready to accept connections', 600000000);
END $$;

-- Step 2: Inject L2 & L3 Automation perfectly inside Layer 1
-- We use a single multi-line file write into the VM!

SELECT vm_send('cat << ''OUTER_EOF'' > /tmp/run_l2.sql
DROP EXTENSION IF EXISTS linuxsql_vm CASCADE;
CREATE EXTENSION linuxsql_vm;

SELECT vm_asset_load(''firmware'', ''/vm/fw_jump.bin'');
SELECT vm_asset_load(''kernel'', ''/vm/kernel.bin'');
SELECT vm_asset_load(''dtb'', ''/vm/linuxsql.dtb'');
SELECT vm_asset_load(''initrd'', ''/vm/initramfs.cpio.gz'');
SELECT vm_boot();

DO $block$
BEGIN
    RAISE NOTICE ''Booting Layer 2 (VM2)...'';
    PERFORM vm_step_until(''database system is ready to accept connections'', 600000000);
END $block$;

-- Now inside Layer 2, instruct it to boot Layer 3!
SELECT vm_send(''cat << ''''INNER_EOF'''' > /tmp/run_l3.sql
DROP EXTENSION IF EXISTS linuxsql_vm CASCADE;
CREATE EXTENSION linuxsql_vm;
SELECT vm_asset_load(''''firmware'''', ''''/vm/fw_jump.bin'''');
SELECT vm_asset_load(''''kernel'''', ''''/vm/kernel.bin'''');
SELECT vm_asset_load(''''dtb'''', ''''/vm/linuxsql.dtb'''');
SELECT vm_asset_load(''''initrd'''', ''''/vm/initramfs.cpio.gz'''');
SELECT vm_boot();
DO \$l3\$
BEGIN
    RAISE NOTICE ''''''Booting Layer 3 (VM3). WE MADE IT!'''''';
    PERFORM vm_step_until(''''linuxsql#'''', 100000000);
END \$l3\$;
SELECT * FROM vm_console_tail(1500);
INNER_EOF
'');

SELECT vm_step(100000); -- Wait for file write
SELECT vm_send(''psql -f /tmp/run_l3.sql'' || chr(10));
SELECT vm_step(100000000); -- Execute L3 inside L2
SELECT * FROM vm_console_tail(2000);
OUTER_EOF
' || chr(10));

SELECT vm_step(150000); -- Let VM1 write the script to disk

-- NOW tell Layer 1 PostgreSQL to execute it
SELECT vm_send('psql -f /tmp/run_l2.sql' || chr(10));

-- Run Layer 1 for enough cycles to completely boot L2, extract L3, and boot L3
DO $$
BEGIN
    RAISE NOTICE 'Executing Deep Triple Inception... Hold on to your hats.';
    PERFORM vm_step(8000000000); 
    RAISE NOTICE 'Inception triple-step completed.';
END $$;

-- Tail the Host VM's console, which contains L2's console, which contains L3's console
SELECT * FROM vm_console_tail(5000);
