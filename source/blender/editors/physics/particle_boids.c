/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2009 Janne Karhu. All rights reserved. */

/** \file
 * \ingroup edphys
 */

#include <stdlib.h>

#include "MEM_guardedalloc.h"

#include "DNA_particle_types.h"

#include "BLI_listbase.h"
#include "BLI_utildefines.h"

#include "BKE_boids.h"
#include "BKE_context.h"
#include "BKE_main.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_enum_types.h"
#include "RNA_prototypes.h"

#include "WM_api.h"
#include "WM_types.h"

#include "physics_intern.h"

/************************ add/del boid rule operators *********************/
static int rule_add_exec(bContext *C, wmOperator *op)
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "particle_settings", &RNA_ParticleSettings);
  ParticleSettings *part = ptr.data;
  int type = RNA_enum_get(op->ptr, "type");

  BoidRule *rule;
  BoidState *state;

  if (!part || part->phystype != PART_PHYS_BOIDS) {
    return OPERATOR_CANCELLED;
  }

  state = boid_get_current_state(part->boids);

  for (rule = state->rules.first; rule; rule = rule->next) {
    rule->flag &= ~BOIDRULE_CURRENT;
  }

  rule = boid_new_rule(type);
  rule->flag |= BOIDRULE_CURRENT;

  BLI_addtail(&state->rules, rule);

  DEG_id_tag_update(&part->id, ID_RECALC_GEOMETRY | ID_RECALC_PSYS_RESET);

  return OPERATOR_FINISHED;
}

void BOID_OT_rule_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Boid Rule";
  ot->description = "Add a boid rule to the current boid state";
  ot->idname = "BOID_OT_rule_add";

  /* api callbacks */
  ot->invoke = WM_menu_invoke;
  ot->exec = rule_add_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna, "type", rna_enum_boidrule_type_items, 0, "Type", "");
}
static int rule_del_exec(bContext *C, wmOperator *UNUSED(op))
{
  Main *bmain = CTX_data_main(C);
  PointerRNA ptr = CTX_data_pointer_get_type(C, "particle_settings", &RNA_ParticleSettings);
  ParticleSettings *part = ptr.data;
  BoidRule *rule;
  BoidState *state;

  if (!part || part->phystype != PART_PHYS_BOIDS) {
    return OPERATOR_CANCELLED;
  }

  state = boid_get_current_state(part->boids);

  for (rule = state->rules.first; rule; rule = rule->next) {
    if (rule->flag & BOIDRULE_CURRENT) {
      BLI_remlink(&state->rules, rule);
      MEM_freeN(rule);
      break;
    }
  }
  rule = state->rules.first;

  if (rule) {
    rule->flag |= BOIDRULE_CURRENT;
  }

  DEG_relations_tag_update(bmain);
  DEG_id_tag_update(&part->id, ID_RECALC_GEOMETRY | ID_RECALC_PSYS_RESET);

  return OPERATOR_FINISHED;
}

void BOID_OT_rule_del(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Remove Boid Rule";
  ot->idname = "BOID_OT_rule_del";
  ot->description = "Delete current boid rule";

  /* api callbacks */
  ot->exec = rule_del_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/************************ move up/down boid rule operators *********************/
static int rule_move_up_exec(bContext *C, wmOperator *UNUSED(op))
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "particle_settings", &RNA_ParticleSettings);
  ParticleSettings *part = ptr.data;
  BoidRule *rule;
  BoidState *state;

  if (!part || part->phystype != PART_PHYS_BOIDS) {
    return OPERATOR_CANCELLED;
  }

  state = boid_get_current_state(part->boids);
  for (rule = state->rules.first; rule; rule = rule->next) {
    if (rule->flag & BOIDRULE_CURRENT && rule->prev) {
      BLI_remlink(&state->rules, rule);
      BLI_insertlinkbefore(&state->rules, rule->prev, rule);

      DEG_id_tag_update(&part->id, ID_RECALC_GEOMETRY | ID_RECALC_PSYS_RESET);
      break;
    }
  }

  return OPERATOR_FINISHED;
}

void BOID_OT_rule_move_up(wmOperatorType *ot)
{
  ot->name = "Move Up Boid Rule";
  ot->description = "Move boid rule up in the list";
  ot->idname = "BOID_OT_rule_move_up";

  ot->exec = rule_move_up_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static int rule_move_down_exec(bContext *C, wmOperator *UNUSED(op))
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "particle_settings", &RNA_ParticleSettings);
  ParticleSettings *part = ptr.data;
  BoidRule *rule;
  BoidState *state;

  if (!part || part->phystype != PART_PHYS_BOIDS) {
    return OPERATOR_CANCELLED;
  }

  state = boid_get_current_state(part->boids);
  for (rule = state->rules.first; rule; rule = rule->next) {
    if (rule->flag & BOIDRULE_CURRENT && rule->next) {
      BLI_remlink(&state->rules, rule);
      BLI_insertlinkafter(&state->rules, rule->next, rule);

      DEG_id_tag_update(&part->id, ID_RECALC_GEOMETRY | ID_RECALC_PSYS_RESET);
      break;
    }
  }

  return OPERATOR_FINISHED;
}

void BOID_OT_rule_move_down(wmOperatorType *ot)
{
  ot->name = "Move Down Boid Rule";
  ot->description = "Move boid rule down in the list";
  ot->idname = "BOID_OT_rule_move_down";

  ot->exec = rule_move_down_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/************************ add/del boid state operators *********************/
static int state_add_exec(bContext *C, wmOperator *UNUSED(op))
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "particle_settings", &RNA_ParticleSettings);
  ParticleSettings *part = ptr.data;
  BoidState *state;

  if (!part || part->phystype != PART_PHYS_BOIDS) {
    return OPERATOR_CANCELLED;
  }

  for (state = part->boids->states.first; state; state = state->next) {
    state->flag &= ~BOIDSTATE_CURRENT;
  }

  state = boid_new_state(part->boids);
  state->flag |= BOIDSTATE_CURRENT;

  BLI_addtail(&part->boids->states, state);

  return OPERATOR_FINISHED;
}

void BOID_OT_state_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Boid State";
  ot->description = "Add a boid state to the particle system";
  ot->idname = "BOID_OT_state_add";

  /* api callbacks */
  ot->exec = state_add_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}
static int state_del_exec(bContext *C, wmOperator *UNUSED(op))
{
  Main *bmain = CTX_data_main(C);
  PointerRNA ptr = CTX_data_pointer_get_type(C, "particle_settings", &RNA_ParticleSettings);
  ParticleSettings *part = ptr.data;
  BoidState *state;

  if (!part || part->phystype != PART_PHYS_BOIDS) {
    return OPERATOR_CANCELLED;
  }

  for (state = part->boids->states.first; state; state = state->next) {
    if (state->flag & BOIDSTATE_CURRENT) {
      BLI_remlink(&part->boids->states, state);
      MEM_freeN(state);
      break;
    }
  }

  /* there must be at least one state */
  if (!part->boids->states.first) {
    state = boid_new_state(part->boids);
    BLI_addtail(&part->boids->states, state);
  }
  else {
    state = part->boids->states.first;
  }

  state->flag |= BOIDSTATE_CURRENT;

  DEG_relations_tag_update(bmain);
  DEG_id_tag_update(&part->id, ID_RECALC_GEOMETRY | ID_RECALC_PSYS_RESET);

  return OPERATOR_FINISHED;
}

void BOID_OT_state_del(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Remove Boid State";
  ot->idname = "BOID_OT_state_del";
  ot->description = "Delete current boid state";

  /* api callbacks */
  ot->exec = state_del_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/************************ move up/down boid state operators *********************/
static int state_move_up_exec(bContext *C, wmOperator *UNUSED(op))
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "particle_settings", &RNA_ParticleSettings);
  ParticleSettings *part = ptr.data;
  BoidSettings *boids;
  BoidState *state;

  if (!part || part->phystype != PART_PHYS_BOIDS) {
    return OPERATOR_CANCELLED;
  }

  boids = part->boids;

  for (state = boids->states.first; state; state = state->next) {
    if (state->flag & BOIDSTATE_CURRENT && state->prev) {
      BLI_remlink(&boids->states, state);
      BLI_insertlinkbefore(&boids->states, state->prev, state);
      break;
    }
  }

  return OPERATOR_FINISHED;
}

void BOID_OT_state_move_up(wmOperatorType *ot)
{
  ot->name = "Move Up Boid State";
  ot->description = "Move boid state up in the list";
  ot->idname = "BOID_OT_state_move_up";

  ot->exec = state_move_up_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static int state_move_down_exec(bContext *C, wmOperator *UNUSED(op))
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "particle_settings", &RNA_ParticleSettings);
  ParticleSettings *part = ptr.data;
  BoidSettings *boids;
  BoidState *state;

  if (!part || part->phystype != PART_PHYS_BOIDS) {
    return OPERATOR_CANCELLED;
  }

  boids = part->boids;

  for (state = boids->states.first; state; state = state->next) {
    if (state->flag & BOIDSTATE_CURRENT && state->next) {
      BLI_remlink(&boids->states, state);
      BLI_insertlinkafter(&boids->states, state->next, state);
      DEG_id_tag_update(&part->id, ID_RECALC_GEOMETRY | ID_RECALC_PSYS_RESET);
      break;
    }
  }

  return OPERATOR_FINISHED;
}

void BOID_OT_state_move_down(wmOperatorType *ot)
{
  ot->name = "Move Down Boid State";
  ot->description = "Move boid state down in the list";
  ot->idname = "BOID_OT_state_move_down";

  ot->exec = state_move_down_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}
