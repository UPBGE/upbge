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

struct PythonComponent *new_component_from_module_name(char *import);
void copy_components(struct ListBase *lbn, struct ListBase *lbo);
void free_component(struct PythonComponent *pc);
void free_components(struct ListBase *base);

#ifdef __cplusplus
}
#endif

#endif /* __BKE_PYTHON_COMPONENT_H__ */
