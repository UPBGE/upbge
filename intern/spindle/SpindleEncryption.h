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

#ifndef _SPINDLEENCRYPTION_H
#define _SPINDLEENCRYPTION_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

extern char *staticKey;
extern char *dynamicKey;

void SpinEncrypt_Hex(char *data, int dataSize, char *key);
void SpinDecrypt_Hex(char *data, int dataSize, char *key);

void SpinSetStaticEncryption_Key(const char *hexKey);
void SpinSetDynamicEncryption_Key(const char *hexKey);

/*
 * We want to define these functions ourselves since some platforms will always dynamically link against
 * libc even if we build a static executable (ex: Linux)
 */
void SpinSecureFunction_Memcpy(void *dest, void *src, int size);
void SpinSecureFunction_Memset(void *dest, char value, int size);

#ifdef __cplusplus
}
#endif

#endif _SPINDLEENCRYPTION_H
