#ifndef _POLICY_H
#define _POLICY_H
#include "structs.h"

/*
 * Single entry point for the STAGE_NEXT event: dispatches t->type to the
 * current policy's read/write implementation, or to the shared train/
 * barrier handlers. Adding a new coherence policy means adding an enum
 * value to policy_t, writing its read/write functions in policy.c, and
 * adding one row to the policies[] table there — nothing here or in
 * main2.c's event_process needs to change.
 */
void policy_dispatch(struct task *t);

/* Display abbreviation for a policy (e.g. "SCU"), or NULL if it has none
 * (FLOW_DEBUG isn't a coherence policy and isn't in the table). */
const char *policy_name(enum policy_t p);

#endif // _POLICY_H
