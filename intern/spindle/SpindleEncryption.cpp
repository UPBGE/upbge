/**
 * ***** BEGIN MIT LICENSE BLOCK *****
 * Copyright (C) 2011-2017 by DeltaSpeeds
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

#include "SpindleEncryption.h"
#include <string.h>
#include <fstream>
#include <iostream>
#include <cstdlib>

char *staticKey = NULL;
char *dynamicKey = NULL;
std::string filePath;
const unsigned int currentSupportedVersion = 0;

// Static functions declaration
// Encryption & Decryption
static void spindle_encrypt(char *data, int dataSize, const unsigned long long key);
static void spindle_decrypt(char *data, int dataSize, const unsigned long long key);
static void spindle_encrypt_hex_64(char *data, int dataSize, const char *key);
static void spindle_decrypt_hex_64(char *data, int dataSize, const char *key);
static void spindle_encrypt_hex(char *data, int dataSize, const char *key);
static void spindle_decrypt_hex(char *data, int dataSize, const char *key);
// Encryption keys
static void spindle_set_static_encryption_key(const char *hexKey);
static void spindle_set_dynamic_encryption_key(const char *hexKey);
// Secure functions
// We want to define these functions ourselves since some platforms will always dynamically link against
// libc even if we build a static executable (ex: Linux)
static void spindle_secure_function_memcpy(void *dest, void *src, int size);
static void spindle_secure_function_memset(void *dest, char value, int size);
static int spindle_secure_function_strlen(const char *str);


std::string SPINDLE_FindAndSetEncryptionKeys(char **argv, int i)
{
	/* Find main key */
	int hexStrSize = 0, argPos = 2, maxStringLen = int(strlen(argv[i]));

	for (hexStrSize = 0; ((argv[i][hexStrSize + argPos] != 0) && (argv[i][hexStrSize + argPos] != '.')); hexStrSize++){}

	char *hexKey = new char[hexStrSize + 1];
	spindle_secure_function_memcpy((char *)hexKey, (char *)&(argv[i][argPos]), hexStrSize);
	spindle_secure_function_memset((char *)&(argv[i][argPos]), 0, hexStrSize);
	hexKey[hexStrSize] = 0;
	argPos += hexStrSize + 1;

	/* Find static key */
	if (argPos < maxStringLen) {
		for (hexStrSize = 0; ((argv[i][hexStrSize + argPos] != 0) && (argv[i][hexStrSize + argPos] != '.')); hexStrSize++){}

		if (hexStrSize > 0) {
			char *statKey = new char[hexStrSize + 1];
			spindle_secure_function_memcpy((char *)statKey, (char *)&(argv[i][argPos]), hexStrSize);
			spindle_secure_function_memset((char *)&(argv[i][argPos]), 0, hexStrSize);
			statKey[hexStrSize] = 0;
			argPos += hexStrSize + 1;
			spindle_set_static_encryption_key(statKey);
			memset((char *)statKey, 0, hexStrSize);
			delete [] statKey;
		}
	}

	/* Find dynamic key */
	if (argPos < maxStringLen) {
		for (hexStrSize = 0; ((argv[i][hexStrSize + argPos] != 0) && (argv[i][hexStrSize + argPos] != '.')); hexStrSize++){}

		if (hexStrSize > 0) {
			char *dynaKey = new char[hexStrSize + 1];
			spindle_secure_function_memcpy((char *)dynaKey, (char *)&(argv[i][argPos]), hexStrSize);
			spindle_secure_function_memset((char *)&(argv[i][argPos]), 0, hexStrSize);
			dynaKey[hexStrSize] = 0;
			argPos += hexStrSize + 1;
			spindle_set_dynamic_encryption_key(dynaKey);
			memset((char *)dynaKey, 0, hexStrSize);
			delete [] dynaKey;
		}
	}
	return hexKey;
}

char *SPINDLE_DecryptFromFile(const char *filename, int *fileSize, const char *encryptKey, int typeEncryption)
{
	std::ifstream inFile(filename, std::ios::in | std::ios::binary | std::ios::ate);
	*fileSize = (int)inFile.tellg();
	if (*fileSize <= 10)
		return NULL;

	if (encryptKey) {
		inFile.seekg(0, std::ios::beg);
		char *fileData = new char[*fileSize];
		inFile.read(fileData, *fileSize);
		inFile.close();
		if ((fileData[0] != 'B')||(fileData[1] != 'L')||(fileData[2] != 'E')||(fileData[3] != 'N')||(fileData[4] != 'D')) {
			spindle_decrypt_hex(fileData, *fileSize, encryptKey);
			return fileData;
		}
		delete[] fileData;
		return NULL;
	}
	else {
		if (typeEncryption == SPINDLE_NO_ENCRYPTION) {
			inFile.seekg(0, std::ios::beg);
			char *fileData = new char[*fileSize];
			inFile.read(fileData, *fileSize);
			inFile.close();
			return fileData;
		}
		else if (typeEncryption == SPINDLE_STATIC_ENCRYPTION) {
			inFile.seekg(5, std::ios::beg);
			*fileSize -= 5;
			char *fileData = new char[*fileSize];
			inFile.read(fileData, *fileSize);
			inFile.close();
			spindle_decrypt_hex(fileData, *fileSize, staticKey);
			return fileData;
		}
		else if (typeEncryption == SPINDLE_DYNAMIC_ENCRYPTION) {
			inFile.seekg(5, std::ios::beg);
			*fileSize -= 5;
			char *fileData = new char[*fileSize];
			inFile.read(fileData, *fileSize);
			inFile.close();
			spindle_decrypt_hex(fileData, *fileSize, dynamicKey);
			return fileData;
		}
		else {
			return NULL;
		}
	}
}

int SPINDLE_CheckEncryptionFromFile(const char *filepath)
{
	int keyType = SPINDLE_NO_ENCRYPTION; // -1 = invalid, 0 = blend, 1 = static key, 2 = dynamic key
	std::ifstream inFile(filepath, std::ios::in | std::ios::binary | std::ios::ate);
	int fileSize = (int)inFile.tellg();
	char *fileData = new char[5];

	if (fileSize < 5) {
		inFile.close();
		return -1;
	}

	inFile.seekg(0, std::ios::beg);
	inFile.read(fileData, 5);

	if ((fileData[0] == 'S') && (fileData[1] == 'T') && (fileData[2] == 'C')) { //Static encrypted file
		if ((unsigned int)fileData[3] > currentSupportedVersion) {
			inFile.close();
			std::cout << "Failed to read blend file: " << filepath << ", blend is from a newer version" << std::endl;
			return -1;
		}
		if (staticKey == NULL) {
			inFile.close();
			std::cout << "Failed to read blend file: " << filepath << ", No static key provided" << std::endl;
			return -1;
		}
		keyType = SPINDLE_STATIC_ENCRYPTION;
	}
	else if ((fileData[0] == 'D') && (fileData[1] == 'Y') && (fileData[2] == 'C')) { //Dynamic encrypted file
		if ((unsigned int)fileData[3] > currentSupportedVersion) {
			inFile.close();
			std::cout << "Failed to read blend file: " << filepath << ", blend is from a newer version" << std::endl;
			return -1;
		}
		if (dynamicKey == NULL) {
			inFile.close();
			std::cout << "Failed to read blend file: " << filepath << ", No dynamic key provided" << std::endl;
			return -1;
		}
		keyType = SPINDLE_DYNAMIC_ENCRYPTION;
	}
	inFile.close();

	return keyType;
}

void SPINDLE_SetFilePath(std::string path)
{
	filePath = path;
}

void SPINDLE_SetFilePath(const char *filepath)
{
	std::string temp(filepath);
	filePath = temp;
}

const char *SPINDLE_GetFilePath()
{
	return filePath.c_str();
}

static void spindle_encrypt(char *data, int dataSize, const unsigned long long key)
{
	const int keySize = sizeof(key) * 8;
	unsigned long long pieceSize, offset, end, chunkSize = 0, max = ((unsigned long long)(dataSize) << 3);
	unsigned int p, t, ii;
	long long i;
	int iii;
	char h;

	for (iii = 0; iii < (keySize >> 4); iii++) {
		p = iii * 16;
		pieceSize = (((key >> p) % (1 << 8)) + 3) * (dataSize / 256 / 400 + 1);
		offset = ((key >> (p + 8)) % (1 << 8)); 
		if (offset == 0) {
			offset++;
		}
		//cout << "(Encrypt) pieceSize: " << pieceSize << " offset: " << offset << " " << p << "\n";
		end = ((unsigned long long)(dataSize) << 3) - (((unsigned long long)(dataSize) << 3) % pieceSize) - pieceSize * ((int)((((unsigned long long)(dataSize) << 3) % pieceSize) == 0));
		for (i = end; i >= 0; i -= pieceSize) {
			chunkSize = pieceSize;
			if (i + chunkSize >= max ) {
				chunkSize = ((unsigned long long)(dataSize) << 3) - i;
			}
			//cout << "   N: " << i << " " << i + chunkSize << "\n";
			t = (unsigned int)((unsigned long long)(i + chunkSize) >> 3);
			ii = (unsigned int)(i >> 3);
			h = ((char)offset) * ((char)i) + ((char)i) - ((char)(pieceSize&i)) + (((char)(offset))|((char)(i))) + ((((char)(iii))|pieceSize)&255);
			while (ii < t) {
				data[ii] += h + (((char)offset) | ((char)ii));
				ii++;
			}
		}
	}
}

static void spindle_decrypt(char *data, int dataSize, const unsigned long long key)
{
	const int keySize = sizeof(key) * 8;
	unsigned long long pieceSize, offset, chunkSize = 0, max = ((unsigned long long)(dataSize) << 3);
	unsigned int p, t, ii;
	unsigned long long i;
	int iii;
	char h;

	for (iii = (keySize >> 4) - 1; iii >= 0; iii--) {
		p = iii * 16;
		pieceSize = (((key >> p) % (1 << 8)) + 3) * (dataSize / 256 / 400 + 1);
		offset = ((key >> (p + 8)) % (1 << 8));
		if (offset == 0) {
			offset++;
		}
		chunkSize = pieceSize;
		//cout << "(Decrypt) pieceSize: " << pieceSize << " offset: " << offset << " " << p << "\n";
		for (i = 0; i < max; i += chunkSize) {
			if (i + chunkSize >= max ) {
				chunkSize = (((unsigned long long)(dataSize)) << 3) - i;
			}
			//cout << "   N: " << i << " " << i + chunkSize << "\n";
			t = (unsigned int)(((unsigned long long)(i + chunkSize)) >> 3);
			ii = (unsigned int)(i >> 3);
			h = ((char)offset) * ((char)i) + ((char)i) - ((char)(pieceSize&i)) + (((char)(offset)) | ((char)(i))) + ((((char)(iii)) | pieceSize)&255);
			while (ii < t) {
				data[ii] -= h + (((char)offset) | ((char)ii));
				ii++;
			}
		}
		chunkSize = pieceSize;
	}
}

static void spindle_encrypt_hex_64(char *data, int dataSize, const char *key)
{
	int keySize = 0, i;
	unsigned long long realKey = 0, s;
	if (key == NULL)
		return;
	keySize = spindle_secure_function_strlen(key);
	for (i = 0; i < keySize; i++) {
		s = keySize - 1 - i;
		if ((key[i] >= '0') && (key[i] <= '9'))
			realKey += ((unsigned long long)(key[i] - '0') << (s << 2));
		else if ((key[i] >= 'a') && (key[i] <= 'f'))
			realKey += ((unsigned long long)(key[i] - 'a' + 10) << (s << 2));
		else if ((key[i] >= 'A') && (key[i] <= 'F'))
			realKey += ((unsigned long long)(key[i] - 'A' + 10) << (s << 2));
		else
			realKey += ((unsigned long long)(key[i]) << (s << 2));
	}
	spindle_encrypt(data, dataSize, realKey);
}

static void spindle_decrypt_hex_64(char *data, int dataSize, const char *key)
{
	int keySize = 0, i;
	unsigned long long realKey = 0, s;
	if (key == NULL)
		return;
	keySize = spindle_secure_function_strlen(key);
	for (i = 0; i < keySize; i++) {
		s = keySize - 1 - i;
		if ((key[i] >= '0') && (key[i] <= '9'))
			realKey += ((unsigned long long)(key[i] - '0') << (s << 2));
		else if ((key[i] >= 'a') && (key[i] <= 'f'))
			realKey += ((unsigned long long)(key[i] - 'a' + 10) << (s << 2));
		else if ((key[i] >= 'A') && (key[i] <= 'F'))
			realKey += ((unsigned long long)(key[i] - 'A' + 10) << (s << 2));
		else
			realKey += ((unsigned long long)(key[i]) << (s << 2));
	}
	spindle_decrypt(data, dataSize, realKey);
}

static void spindle_encrypt_hex(char *data, int dataSize, const char *key)
{
	int keySize = 0, charPos, i;
	if (key == NULL)
		return;
	keySize = spindle_secure_function_strlen(key);
	charPos = keySize - (keySize % 16);
	if (keySize == charPos)
		charPos -= 16;
	if (keySize <= 16) {
		spindle_encrypt_hex_64(data, dataSize, key);
	}
	else {
		char tempKey[17];
		tempKey[16] = 0;
		while (charPos >= 0) {
			for (i = 0; ((i < 16) && (i < keySize)); i++) {
				tempKey[i] = key[i + charPos];
			}
			charPos -= 16;
			spindle_encrypt_hex_64(data, dataSize, tempKey);
		}
	}
}

static void spindle_decrypt_hex(char *data, int dataSize, const char *key)
{
	int keySize = 0, charPos = 0, i;
	if (key == NULL)
		return;
	keySize = spindle_secure_function_strlen(key);
	if (keySize <= 16) {
		spindle_decrypt_hex_64(data, dataSize, key);
	}
	else {
		char tempKey[17];
		tempKey[16] = 0;
		while (charPos < keySize) {
			for (i = 0; ((i < 16) && (i < keySize)); i++) {
				tempKey[i] = key[i + charPos];
			}
			charPos += 16;
			spindle_decrypt_hex_64(data, dataSize, tempKey);
		}
	}
}

static void spindle_set_static_encryption_key(const char *hexKey)
{
	if (staticKey != NULL)
		free(staticKey);
	staticKey = (char *)malloc((int)strlen(hexKey) + 1);
	strcpy(staticKey, hexKey);
}

static void spindle_set_dynamic_encryption_key(const char *hexKey)
{
	if (dynamicKey != NULL)
		free(dynamicKey);
	dynamicKey = (char *)malloc((int)strlen(hexKey) + 1);
	strcpy(dynamicKey, hexKey);
}

static void spindle_secure_function_memcpy(void *dest, void *src, int size)
{
	for (int i = 0; i < size; i++)
		((char *)dest)[i] = ((char *)src)[i];
}

static void spindle_secure_function_memset(void *dest, char value, int size)
{
	for (int i = 0; i < size; i++)
		((char *)dest)[i] = value;
}

static int spindle_secure_function_strlen(const char *str)
{
	int val = 0;
	if (str == NULL)
		return 0;
	while (str[val] != 0)
		val++;
	return val;
}
