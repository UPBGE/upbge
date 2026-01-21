/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sptext
 *
 * TypeScript/JavaScript LSP (typescript-language-server) integration for
 * autocomplete in the text editor.
 */

#pragma once

namespace blender {

struct Text;
struct TextFormatType;

/** Return true if \a tft is the JS/TS formatter (extensions js, mjs, cjs, ts, mts, cts). */
bool text_format_is_js_or_ts(const TextFormatType *tft);

/**
 * Try to get completions from the TypeScript LSP. Fills texttool_suggest_* on success.
 * \return true if at least one suggestion was added.
 */
bool ts_lsp_get_completions(Text *text, const char *seek, int seek_len, TextFormatType *tft);

/** Shut down the LSP server and free resources. Safe to call if never started. */
void ts_lsp_shutdown(void);

}  // namespace blender
