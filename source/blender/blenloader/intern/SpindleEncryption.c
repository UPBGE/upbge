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
*
*/
#include "stdafx.h"
#include "SpindleEncryption.h"

int strlenN(char* str);
void SpinEncrypt_Hex_64(char* data, int dataSize, char* key);
void SpinDecrypt_Hex_64(char* data, int dataSize, char* key);


int strlenN(char* str)
{
	int val = 0;
	if (str == NULL)
		return 0;
	while (str[val] != 0)
		val++;
	return val;
}

void circularShift(char* data, unsigned long long startPos, unsigned long long endPos, unsigned long long shiftAmount, char shiftRight)
{
	unsigned long long chunkSize, j, i, base;
	char b, bNext;
	i = 0;
	chunkSize = endPos - startPos;
	shiftAmount %= chunkSize;
	//if (shiftAmount == 0){shiftAmount = 1;}
	if ((shiftAmount == 0)||(chunkSize <= 1)){return;}
	if (!shiftRight)
		shiftAmount = chunkSize - shiftAmount;

	j = startPos + shiftAmount;
	base = startPos;
	if (j >= endPos){j = (j%endPos) + startPos;}
	b = (((((unsigned char)(data[startPos>>3])) >> (7-(startPos&7)))&1) == 1);

	while (i < chunkSize)
	{
		/*for (int jk = 0; jk < chunkSize; jk++)
			cout << int((((unsigned char)(data[jk/8])) >> (7-(jk%8)))%2);
		cout << "  " << i << "  " << j << "\n";*/
		//Read bit
		bNext = (((((unsigned char)(data[j>>3])) >> (7-(j&7)))&1) == 1);

		//Write bit
		if (bNext)
		{
			if (!b)
				data[j>>3] = (unsigned char)(data[j>>3]) - (1 << (7-(j&7)));
		}
		else if (b)
			data[j>>3] = (unsigned char)(data[j>>3]) + (1 << (7-(j&7)));

		b = bNext;
		if ((j >= startPos)&&(j <= base)){
		while ((j >= startPos)&&(j <= base))
			j++;
		b = (((((unsigned char)(data[j>>3])) >> (7-(j&7)))&1) == 1);
		base = j;}
		j += shiftAmount;
		if (j >= endPos){j = (j%endPos) + startPos;}
		i++;
	}
}

void SpinEncrypt(char* data, int dataSize, unsigned long long key)
{
	const int keySize = sizeof(key)*8;
	unsigned long long pieceSize, offset, end, chunkSize = 0, max = ((unsigned long long)(dataSize)<<3);
	unsigned int p, t, ii;
	long long i;
	int iii;
	char h;

	for (iii = 0; iii < (keySize>>4); iii++)
	{
		p = iii*16; //keySize-1-(iii*16 + 8);
		pieceSize = (((key >> p)%(1<<8)) + 3)*(dataSize/256/400 + 1); //if (pieceSize == chunkSize){pieceSize++;}
		offset = ((key >> (p + 8))%(1<<8)); if (offset == 0){offset++;}
		//cout << "(Encrypt) pieceSize: " << pieceSize << " offset: " << offset << " " << p << "\n";
		end = ((unsigned long long)(dataSize)<<3) - (((unsigned long long)(dataSize)<<3)%pieceSize) - pieceSize*((int)( (((unsigned long long)(dataSize)<<3)%pieceSize) == 0 ));
		//if (end == (unsigned long long(dataSize)<<3)){end -= pieceSize;}
		//for (unsigned long long i = 0; i < (unsigned long long(dataSize)<<3); i += chunkSize)
		for (i = end; i >= 0; i -= pieceSize)
		{
			chunkSize = pieceSize;
			if (i+chunkSize >= max ){chunkSize = ((unsigned long long)(dataSize)<<3) - i;}
			//cout << "   N: " << i << " " << i + chunkSize << "\n";
			t = (unsigned int)((unsigned long long)(i + chunkSize)>>3);
			ii = (unsigned int)(i>>3);
			h = ((char)offset)*((char)i) + ((char)i) - ((char)(pieceSize&i)) + (((char)(offset))|((char)(i))) + ((((char)(iii))|pieceSize)&255);
			//for (unsigned int ii = i>>3; ii < t; ii++)
			while (ii < t)
			{
				data[ii] += h + (((char)offset)|((char)ii));//char(ii) - h + (char(offset)|char(ii));
				ii++;
			}
			//circularShift(data, i, i+chunkSize, offset, true);
		}
	}
}

void SpinDecrypt(char* data, int dataSize, unsigned long long key)
{
	const int keySize = sizeof(key)*8;
	unsigned long long pieceSize, offset, chunkSize = 0, max = ((unsigned long long)(dataSize)<<3);
	unsigned int p, t, ii;
	unsigned long long i;
	int iii;
	char h;

	for (iii = (keySize>>4)-1; iii >= 0; iii--)
	{
		p = iii*16; //keySize-1-(iii*16 + 8);
		pieceSize = (((key >> p)%(1<<8)) + 3)*(dataSize/256/400 + 1); //if (pieceSize == chunkSize){pieceSize++;}
		offset = ((key >> (p + 8))%(1<<8)); if (offset == 0){offset++;}
		chunkSize = pieceSize;
		//cout << "(Decrypt) pieceSize: " << pieceSize << " offset: " << offset << " " << p << "\n";
		for (i = 0; i < max; i += chunkSize)
		{
			if (i+chunkSize >= max ){chunkSize = (((unsigned long long)(dataSize))<<3) - i;}
			//circularShift(data, i, i+chunkSize, offset, false);
			//cout << "   N: " << i << " " << i + chunkSize << "\n";
			t = (unsigned int)(((unsigned long long)(i + chunkSize))>>3);
			ii = (unsigned int)(i>>3);
			//for (unsigned int ii = i>>3; ii < t; ii++)
			h = ((char)offset)*((char)i) + ((char)i) - ((char)(pieceSize&i)) + (((char)(offset))|((char)(i))) + ((((char)(iii))|pieceSize)&255);
			while (ii < t)
			{
				data[ii] -= h + (((char)offset)|((char)ii));//char(ii) - h + (char(offset)|char(ii));
				ii++;
			}
		}
		chunkSize = pieceSize;
	}
}

void SpinEncrypt_Hex_64(char* data, int dataSize, char* key)
{
	int keySize = 0, i;
	unsigned long long realKey = 0, s;
   if (key == NULL)
      return;
	keySize = strlenN(key);
	for (i = 0; i < keySize; i++)
	{
		s = keySize - 1 - i;
		if ((key[i] >= '0')&&(key[i] <= '9'))
			realKey += ((unsigned long long)(key[i]-'0')<<(s<<2));
		else if ((key[i] >= 'a')&&(key[i] <= 'f'))
			realKey += ((unsigned long long)(key[i]-'a'+10)<<(s<<2));
		else if ((key[i] >= 'A')&&(key[i] <= 'F'))
			realKey += ((unsigned long long)(key[i]-'A'+10)<<(s<<2));
		else
			realKey += ((unsigned long long)(key[i])<<(s<<2));
	}
	SpinEncrypt(data, dataSize, realKey);
}

void SpinDecrypt_Hex_64(char* data, int dataSize, char* key)
{
	int keySize = 0, i;
	unsigned long long realKey = 0, s;
   if (key == NULL)
      return;
	keySize = strlenN(key);
	for (i = 0; i < keySize; i++)
	{
		s = keySize - 1 - i;
		if ((key[i] >= '0')&&(key[i] <= '9'))
			realKey += ((unsigned long long)(key[i]-'0')<<(s<<2));
		else if ((key[i] >= 'a')&&(key[i] <= 'f'))
			realKey += ((unsigned long long)(key[i]-'a'+10)<<(s<<2));
		else if ((key[i] >= 'A')&&(key[i] <= 'F'))
			realKey += ((unsigned long long)(key[i]-'A'+10)<<(s<<2));
		else
			realKey += ((unsigned long long)(key[i])<<(s<<2));
	}
	SpinDecrypt(data, dataSize, realKey);
}


void SpinEncrypt_Hex(char* data, int dataSize, char* key)
{
	int keySize = 0, charPos, i;
	if (key == NULL)
      return;
   keySize = strlenN(key);
   charPos = keySize - (keySize % 16);
	if (keySize == charPos)
		charPos -= 16;
	if (keySize <= 16)
		SpinEncrypt_Hex_64(data, dataSize, key);
	else
	{
		char tempKey[17];
		tempKey[16] = 0;
		while (charPos >= 0)
		{
			for (i = 0; ((i < 16)&&(i < keySize)); i++)
				tempKey[i] = key[i+charPos];
			charPos -= 16;
			SpinEncrypt_Hex_64(data, dataSize, tempKey);
		}
	}
}

void SpinDecrypt_Hex(char* data, int dataSize, char* key)
{
	int keySize = 0, charPos = 0, i;
	if (key == NULL)
      return;
   keySize = strlenN(key);
   if (keySize <= 16)
		SpinDecrypt_Hex_64(data, dataSize, key);
	else
	{
		char tempKey[17];
		tempKey[16] = 0;
		while (charPos < keySize)
		{
			for (i = 0; ((i < 16)&&(i < keySize)); i++)
				tempKey[i] = key[i+charPos];
			charPos += 16;
			SpinDecrypt_Hex_64(data, dataSize, tempKey);
		}
	}
}
