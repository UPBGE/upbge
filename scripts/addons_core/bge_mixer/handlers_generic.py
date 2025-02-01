"""
This module defines Blender handlers for Mixer in generic synchronization mode
"""
from __future__ import annotations

import logging
from typing import Set, Tuple

import bpy
import bpy.types as T  # noqa N812

from mixer.blender_client import data as data_api
from mixer.blender_data.diff import BpyBlendDiff
from mixer.blender_data.filter import safe_properties


from mixer.share_data import share_data

logger = logging.getLogger(__name__)


def extra_updates(updates: Set[T.ID]) -> Set[T.ID]:
    extra_updates = set()
    for datablock in updates:
        if hasattr(datablock, "shape_keys") and isinstance(datablock.shape_keys, T.Key):
            # in some cases (TestShapeKey.test_rename_key), the Key update is missing. Always check for shape_keys
            extra_updates.add(datablock.shape_keys)
        if isinstance(datablock, T.Scene) and datablock.grease_pencil is not None:
            # scene annotation change do not trigger a DG update
            extra_updates.add(datablock.grease_pencil)

    return updates.union(extra_updates)


def updates_to_check(depsgraph: T.Depsgraph) -> Tuple[Set[T.ID], Set[T.ID]]:
    """Determines the list of datablocks to check.

    This mostly implements tweaks to add missing updates and remove extranous ones
    Args:
    - depsgraph: the dependency graph that triggered the update
    Returns:
    - a tuple with a set of datablocks to check at ones, and a set of datablocks to check later when changing mode
    """
    updates = {update.id.original for update in depsgraph.updates}

    # Delay the update of Object data to avoid Mesh updates in edit or paint mode, but keep other updates.
    # Mesh separate delivers Collection as well as created Object and Mesh updates while the edited
    # object is in edit mode, and these updates are not delivered when leaving edit mode, so
    # make sure to process them anyway. It is also possible to edit multiple objects at once

    # When no Object is selected and a Mesh is selected the Object with the selected Mesh is the
    # active_object, but not in selected_objects
    current_objects = set(getattr(bpy.context, "selected_objects", []))
    active_object = getattr(bpy.context, "active_object", None)
    if active_object:
        current_objects.add(active_object)

    delayed_updates = set()
    for datablock in updates:
        if datablock in current_objects and datablock.mode != "OBJECT" and datablock.data is not None:
            delayed_updates.add(datablock)
            delayed_updates.add(datablock.data)

    updates = extra_updates(updates)
    delayed_updates = extra_updates(delayed_updates)

    if bpy.context.mode == "POSE":
        # moving a controller triggers way too many updates. Defer everything
        delayed_updates.update(updates)

    updates -= delayed_updates

    if logger.getEffectiveLevel() <= logging.INFO:
        logger.info("send_scene_data_to_server. Delayed updates ")
        for update in delayed_updates:
            logger.info("... %r", update)

        logger.info("send_scene_data_to_server. Actual updates ")
        for update in updates:
            logger.info("... %r", update)

    return updates, delayed_updates


def send_scene_data_to_server(scene, dummy):

    logger.debug(
        "send_scene_data_to_server(): skip_next_depsgraph_update %s, pending_test_update %s",
        share_data.client.skip_next_depsgraph_update,
        share_data.pending_test_update,
    )

    depsgraph = bpy.context.evaluated_depsgraph_get()
    if depsgraph.updates:
        logger.debug(f"DG updates for {depsgraph.scene} {depsgraph.view_layer}")
        for update in depsgraph.updates:
            logger.debug(" ......%r", update.id.original)
    else:
        # FIXME Possible missed update :
        # If an updated datablock is not linked in the current scene/view_layer, the update triggers
        # an empty DG update batch. This can happen when the update is from a script.
        logger.info(f"DG updates empty for {depsgraph.scene} {depsgraph.view_layer}")

    # prevent processing self events, but always process test updates
    if not share_data.pending_test_update and share_data.client.skip_next_depsgraph_update:
        share_data.client.skip_next_depsgraph_update = False
        logger.debug("send_scene_data_to_server canceled (skip_next_depsgraph_update = True) ...")
        return

    share_data.pending_test_update = False
    bpy_data_proxy = share_data.bpy_data_proxy
    depsgraph = bpy.context.evaluated_depsgraph_get()

    updates, delayed_updates = updates_to_check(depsgraph)

    # delayed update processing is delayed until the selected objects return to OBJECT mode
    process_delayed_updates = not delayed_updates

    if delayed_updates:
        bpy_data_proxy.append_delayed_updates(delayed_updates)

    # Compute the difference between the proxy state and the Blender state
    # It is a coarse difference at the ID level(created, removed, renamed)
    diff = BpyBlendDiff()
    diff.diff(bpy_data_proxy, safe_properties)

    # Ask the proxy to compute the list of elements to synchronize and update itself
    changeset = bpy_data_proxy.update(diff, updates, process_delayed_updates, safe_properties)

    # Send creations before update so that collection updates for new object have a valid target
    data_api.send_data_creations(changeset.creations)
    data_api.send_data_removals(changeset.removals)
    data_api.send_data_renames(changeset.renames)
    data_api.send_data_updates(changeset.updates)

    logger.debug("send_scene_data_to_server: end")
