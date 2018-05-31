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

#ifndef __BKE_PYTHON_COMPONENT_H__
#define __BKE_PYTHON_COMPONENT_H__

#ifdef __cplusplus
extern "C" {
#endif

struct PythonComponent *BKE_python_component_new(char *import, struct ReportList *reports, struct bContext *context);
struct PythonComponent *BKE_python_component_create_file(char *import, struct ReportList *reports, struct bContext *context);
void BKE_python_component_reload(struct PythonComponent *pc, struct ReportList *reports, struct bContext *context);
void BKE_python_component_copy_list(struct ListBase *lbn, const struct ListBase *lbo);
void BKE_python_component_free(struct PythonComponent *pc);
void BKE_python_component_free_list(struct ListBase *base);

void *BKE_python_component_argument_dict_new(struct PythonComponent *pc);

#ifdef __cplusplus
}
#endif

#endif /* __BKE_PYTHON_COMPONENT_H__ */
