/**
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Mitchell Stokes, Diego Lopes, Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#pragma once

#include "BKE_lib_query.hh"

#include "DNA_windowmanager_types.h" /* for ReportType */

typedef void (*BKEPyProxyIDFunc)(struct PythonProxy *pp,
                                 struct ID **idpoin,
                                 void *userdata,
                                 LibraryForeachIDCallbackFlag cb_flag);

struct PythonProxy *BKE_custom_object_new(char *import,
                                          struct ReportList *reports,
                                          struct bContext *context);
struct PythonProxy *BKE_custom_object_create_file(char *import,
                                                  struct ReportList *reports,
                                                  struct bContext *context);
void BKE_custom_object_reload(struct PythonProxy *pp,
                              struct ReportList *reports,
                              struct bContext *context);

struct PythonProxy *BKE_python_component_new(char *import,
                                             struct ReportList *reports,
                                             struct bContext *context);
struct PythonProxy *BKE_python_component_create_file(char *import,
                                                     struct ReportList *reports,
                                                     struct bContext *context);
void BKE_python_component_reload(struct PythonProxy *pp,
                                 struct ReportList *reports,
                                 struct bContext *context);

struct PythonProxy *BKE_python_proxy_copy(PythonProxy *pp);

void BKE_python_proxy_copy_list(struct ListBase *lbn, const struct ListBase *lbo);
void BKE_python_proxy_free(struct PythonProxy *pp);
void BKE_python_proxy_free_list(struct ListBase *base);

void BKE_python_proxy_id_loop(struct PythonProxy *pp, BKEPyProxyIDFunc func, void *userdata);

void BKE_python_proxies_id_loop(struct ListBase *complist, BKEPyProxyIDFunc func, void *userdata);

void *BKE_python_proxy_argument_dict_new(struct PythonProxy *pp);
