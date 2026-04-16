-- linuxsql--1.0.sql
-- PostgreSQL extension: Linux kernel as a database

-- Boot the Linux kernel inside this PostgreSQL backend.
-- Returns the kernel boot log as text.
CREATE FUNCTION boot()
RETURNS TEXT
AS 'linuxsql', 'linux_boot'
LANGUAGE C STRICT;

-- Execute a shell command inside the Linux environment.
-- Returns the command's stdout output as text.
CREATE FUNCTION exec(cmd TEXT)
RETURNS TEXT
AS 'linuxsql', 'linux_exec'
LANGUAGE C STRICT;

-- Tables for kernel I/O (created but not populated until boot)

-- Disk: block device storage
CREATE TABLE disk (
    block_id BIGINT PRIMARY KEY,
    data BYTEA NOT NULL
);

-- Framebuffer: display output
CREATE TABLE framebuffer (
    id INTEGER PRIMARY KEY DEFAULT 0,
    width INTEGER DEFAULT 1024,
    height INTEGER DEFAULT 768,
    bpp INTEGER DEFAULT 32,
    pixels BYTEA
);

COMMENT ON FUNCTION boot() IS
    'Boot the Linux 6.1 kernel inside PostgreSQL. Returns kernel boot log.';
COMMENT ON FUNCTION exec(TEXT) IS
    'Execute a shell command via busybox. Returns stdout/stderr output.';
COMMENT ON TABLE disk IS
    'Block device storage for the Linux kernel. Each row is a 4KB disk block.';
COMMENT ON TABLE framebuffer IS
    'Virtual framebuffer for X11 display output.';
