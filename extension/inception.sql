\c linuxsql
DROP EXTENSION IF EXISTS linuxsql_vm CASCADE;
CREATE EXTENSION linuxsql_vm;
\i /path/to/linuxsql/extension/load_assets.sql

-- We boot the FIRST VM 
SELECT vm_boot();
SELECT vm_jit_set(true);

-- Boot Outer VM
DO $$
DECLARE
    res RECORD;
BEGIN
    RAISE NOTICE 'Booting Outer VM...';
    res := vm_step_until('database system is ready to accept connections', 60000000000);
END $$;

-- Drop to a fast execution for inner nested script writing
SELECT vm_send('echo "DROP EXTENSION IF EXISTS linuxsql_vm CASCADE;" > /tmp/run.sql' || chr(10));
SELECT vm_send('echo "CREATE EXTENSION linuxsql_vm;" >> /tmp/run.sql' || chr(10));
SELECT vm_send('echo "SELECT vm_asset_load(''firmware'', ''/vm/fw_jump.bin'');" >> /tmp/run.sql' || chr(10));
SELECT vm_send('echo "SELECT vm_asset_load(''kernel'', ''/vm/kernel.bin'');" >> /tmp/run.sql' || chr(10));
SELECT vm_send('echo "SELECT vm_asset_load(''initrd'', ''/vm/initramfs.cpio.gz'');" >> /tmp/run.sql' || chr(10));
SELECT vm_send('echo "SELECT vm_boot();" >> /tmp/run.sql' || chr(10));
SELECT vm_send('echo "SELECT vm_step(30000000);" >> /tmp/run.sql' || chr(10));
SELECT vm_send('echo "SELECT * FROM vm_console_tail(1500);" >> /tmp/run.sql' || chr(10));

SELECT vm_step(1000000); -- Let it write the script to disk

-- NOW tell outer PostgreSQL to execute it
SELECT vm_send('psql -f /tmp/run.sql' || chr(10));

-- Now, run the Outer VM to process this!
-- This outer step will run for several billion cycles while the Inner VM boots inside the Outer's postgres!
DO $$
BEGIN
    RAISE NOTICE 'Executing Nested Inception...';
    -- Outer emulator runs interpreted inner emulator
    PERFORM vm_step(10000000000); 
    RAISE NOTICE 'Inception step completed.';
END $$;

-- Finally tail the OUTER VM's console. It should contain the psql output, 
-- which in turn contains the INNER VM's console output!!
SELECT * FROM vm_console_tail(2500);
