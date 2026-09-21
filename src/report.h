#ifndef _REPORT_H
#define _REPORT_H

/*
 * End-of-run report, written to the `result` stream: a sectioned summary for
 * reading, then one `result key=value ...` line, always last, carrying every
 * headline figure for scripts. Parse only that line; the layout above it is
 * free to change.
 */
void report_print(const char *topology_file, long wall_ms, long cpu_ms);

#endif /* _REPORT_H */
