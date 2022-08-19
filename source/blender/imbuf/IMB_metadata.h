/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup imbuf
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct IDProperty;
struct ImBuf;
struct anim;

/**
 * The metadata is a list of key/value pairs (both char *) that can me
 * saved in the header of several image formats.
 * Apart from some common keys like
 * 'Software' and 'Description' (PNG standard) we'll use keys within the
 * Blender namespace, so should be called 'Blender::StampInfo' or 'Blender::FrameNum'
 * etc...
 *
 * The keys & values are stored in ID properties, in the group "metadata".
 */

/**
 * Ensure that the metadata property is a valid #IDProperty object.
 * This is a no-op when *metadata != NULL.
 */
void IMB_metadata_ensure(struct IDProperty **metadata);
void IMB_metadata_free(struct IDProperty *metadata);

/**
 * Read the field from the image info into the field.
 * \param metadata: the #IDProperty that contains the metadata
 * \param key: the key of the field
 * \param value: the data in the field, first one found with key is returned,
 *                 memory has to be allocated by user.
 * \param len: length of value buffer allocated by user.
 * \return 1 (true) if metadata is present and value for the key found, 0 (false) otherwise.
 */
bool IMB_metadata_get_field(struct IDProperty *metadata, const char *key, char *value, size_t len);

/**
 * Set user data in the metadata.
 * If the field already exists its value is overwritten, otherwise the field
 * will be added with the given value.
 * \param metadata: the #IDProperty that contains the metadata
 * \param key: the key of the field
 * \param value: the data to be written to the field. zero terminated string
 */
void IMB_metadata_set_field(struct IDProperty *metadata, const char *key, const char *value);

void IMB_metadata_copy(struct ImBuf *dimb, struct ImBuf *simb);
struct IDProperty *IMB_anim_load_metadata(struct anim *anim);

/* Invoke callback for every value stored in the metadata. */
typedef void (*IMBMetadataForeachCb)(const char *field, const char *value, void *userdata);
void IMB_metadata_foreach(struct ImBuf *ibuf, IMBMetadataForeachCb callback, void *userdata);

#ifdef __cplusplus
}
#endif
