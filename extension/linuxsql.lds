/* extension/linuxsql.lds — Linker script fragment for section ordering
 *
 * Ensures scheduler classes are contiguous and in the correct memory order.
 * The kernel checks (in sched_init):
 *   idle == fair + 1
 *   fair == rt + 1
 *   rt   == dl + 1
 *
 * So address order is: dl, rt, fair, idle (ascending).
 * __sched_class_highest = &dl (highest priority at lowest addr)
 * __sched_class_lowest  = &idle + 1 (past the end)
 */
SECTIONS {
  .data.sched_classes : {
    __sched_class_highest = .;
    *(__dl_sched_class)
    *(__rt_sched_class)
    *(__fair_sched_class)
    *(__idle_sched_class)
    __sched_class_lowest = .;
  }
}
INSERT AFTER .data;
