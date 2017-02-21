/**
 * ***** BEGIN MIT LICENSE BLOCK *****
 * Copyright (C) 2011-2012 by DeltaSpeeds
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END MIT LICENSE BLOCK *****
 */

#ifndef __SPINDLEENCRYPTION_H__
#define __SPINDLEENCRYPTION_H__

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <string>

extern char *staticKey;
extern char *dynamicKey;


std::string SPINDLE_FindAndSetEncryptionKeys(char **argv, int i);
char *SPINDLE_DecryptFromFile(char *filename, int &fileSize, const std::string& encryptKey, int typeEncryption=0);
void *SPINDLE_DecryptFromMemory(void *mem, int &memLength, int typeEncryption);
int SPINDLE_CheckHeaderFromFile(const char *filepath);
int SPINDLE_CheckHeaderFromMemory(void *mem);


#endif  // __SPINDLEENCRYPTION_H__
