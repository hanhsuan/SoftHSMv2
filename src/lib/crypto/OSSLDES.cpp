/*
 * Copyright (c) 2010 SURFnet bv
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*****************************************************************************
 OSSLDES.cpp

 OpenSSL (3)DES implementation
 *****************************************************************************/

#include "config.h"
#include "OSSLDES.h"
#include <algorithm>
#include "odd.h"

bool OSSLDES::wrapKey(const SymmetricKey* key, const SymWrap::Type mode, const ByteString& in, ByteString& out)
{
	if ((mode == SymWrap::DES_KEYWRAP || mode == SymWrap::DES_CBC_KEYWRAP) && !checkLength(in.size(), 8, "wrap"))
		return false;

	return wrapUnwrapKey(key, mode, in, out, 1);
}

bool OSSLDES::unwrapKey(const SymmetricKey* key, const SymWrap::Type mode, const ByteString& in, ByteString& out)
{
	if ((mode == SymWrap::DES_KEYWRAP || mode == SymWrap::DES_CBC_KEYWRAP) && !checkLength(in.size(), 8, "unwrap"))
		return false;
        
	return wrapUnwrapKey(key, mode, in, out, 0);
}

bool OSSLDES::checkLength(const int insize, const int minsize, const char * const operation) const
{
	if (insize < minsize)
	{
		ERROR_MSG("key data to %s too small", operation);
		return false;
	}
	if ((insize % 8) != 0)
	{
		ERROR_MSG("key data to %s not aligned", operation);
		return false;
	}
	return true;
}

const EVP_CIPHER* OSSLDES::getWrapCipher(const SymWrap::Type mode, const SymmetricKey* key) const
{
	if (key == NULL)
		return NULL;

	// Determine the un/wrapping mode
	if (mode == SymWrap::DES_KEYWRAP)
	{
		switch(key->getBitLen())
		{
			case 64:
				return EVP_des_ecb();
			case 128:
				return EVP_des_ede_ecb();
			case 192:
				return EVP_des_ede3_ecb();
		};
	}else if(mode == SymWrap::DES_CBC_KEYWRAP){
        switch(key->getBitLen())
		{
			case 64:
				return EVP_des_cbc();
			case 128:
				return EVP_des_ede_cbc();
			case 192:
				return EVP_des_ede3_cbc();
		};
    }

	ERROR_MSG("unknown DES key wrap mode %i", mode);

	return NULL;
}

// EVP wrapping/unwrapping
// wrap = 1 -> wrapping
// wrap = 0 -> unwrapping
bool OSSLDES::wrapUnwrapKey(const SymmetricKey* key, const SymWrap::Type mode, const ByteString& in, ByteString& out, const int wrap) const
{
	const char *prefix = "";
	if (wrap == 0)
		prefix = "un";

	// Determine the cipher method
	const EVP_CIPHER* cipher = getWrapCipher(mode, key);
	if (cipher == NULL)
	{
		ERROR_MSG("Failed to get EVP %swrap cipher", prefix);
		return false;
	}

	// Allocate the EVP context
	EVP_CIPHER_CTX* pWrapCTX = EVP_CIPHER_CTX_new();
	if (pWrapCTX == NULL)
	{
		ERROR_MSG("Failed to allocate space for EVP_CIPHER_CTX");
		return false;
	}
	EVP_CIPHER_CTX_set_flags(pWrapCTX, EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);

	int rv = EVP_CipherInit_ex(pWrapCTX, cipher, NULL, (unsigned char*) key->getKeyBits().const_byte_str(), NULL, wrap);
	if (rv)
		// Padding is handled by cipher mode separately
		rv = EVP_CIPHER_CTX_set_padding(pWrapCTX, 0);
	if (!rv)
	{
		ERROR_MSG("Failed to initialise EVP cipher %swrap operation", prefix);

		EVP_CIPHER_CTX_free(pWrapCTX);
		return false;
	}

	// 1 input byte could be expanded to two AES blocks
	out.resize(in.size() + 2 * EVP_CIPHER_CTX_block_size(pWrapCTX) - 1);
	int outLen = 0;
	int curBlockLen = 0;
	rv = EVP_CipherUpdate(pWrapCTX, &out[0], &curBlockLen, in.const_byte_str(), in.size());
	if (rv == 1) {
		outLen = curBlockLen;
		rv = EVP_CipherFinal_ex(pWrapCTX, &out[0] + outLen, &curBlockLen);
	}
	if (rv != 1)
	{
		ERROR_MSG("Failed EVP %swrap operation", prefix);

		EVP_CIPHER_CTX_free(pWrapCTX);
		return false;
	}
	EVP_CIPHER_CTX_free(pWrapCTX);
	outLen += curBlockLen;
	out.resize(outLen);
	return true;
}

const EVP_CIPHER* OSSLDES::getCipher() const
{
	if (currentKey == NULL) return NULL;

	// Check currentKey bit length; 3DES only supports 56-bit, 112-bit or 168-bit keys 
	if (
#ifndef WITH_FIPS
	    (currentKey->getBitLen() != 56) &&
#endif
	    (currentKey->getBitLen() != 112) &&
            (currentKey->getBitLen() != 168))
	{
		ERROR_MSG("Invalid DES currentKey length (%d bits)", currentKey->getBitLen());

		return NULL;
	}

	// People shouldn't really be using 56-bit DES keys, generate a warning
	if (currentKey->getBitLen() == 56)
	{
		DEBUG_MSG("CAUTION: use of 56-bit DES keys is not recommended!");
	}

	// Determine the cipher mode
	if (currentCipherMode == SymMode::CBC)
	{
		switch(currentKey->getBitLen())
		{
			case 56:
				return EVP_des_cbc();
			case 112:
				return EVP_des_ede_cbc();
			case 168:
				return EVP_des_ede3_cbc();
		};
	}
	else if (currentCipherMode == SymMode::ECB)
	{
		switch(currentKey->getBitLen())
		{
			case 56:
				return EVP_des_ecb();
			case 112:
				return EVP_des_ede_ecb();
			case 168:
				return EVP_des_ede3_ecb();
		};
	}
	else if (currentCipherMode == SymMode::OFB)
	{
		switch(currentKey->getBitLen())
		{
			case 56:
				return EVP_des_ofb();
			case 112:
				return EVP_des_ede_ofb();
			case 168:
				return EVP_des_ede3_ofb();
		};
	}
	else if (currentCipherMode == SymMode::CFB)
	{
		switch(currentKey->getBitLen())
		{
			case 56:
				return EVP_des_cfb();
			case 112:
				return EVP_des_ede_cfb();
			case 168:
				return EVP_des_ede3_cfb();
		};
	}

	ERROR_MSG("Invalid DES cipher mode %i", currentCipherMode);

	return NULL;
}

bool OSSLDES::generateKey(SymmetricKey& key, RNG* rng /* = NULL */)
{
	if (rng == NULL)
	{
		return false;
	}

	if (key.getBitLen() == 0)
	{
		return false;
	}

	ByteString keyBits;

	// don't count parity bit
	if (!rng->generateRandom(keyBits, key.getBitLen()/7))
	{
		return false;
	}

	// fix the odd parity
	size_t i;
	for (i = 0; i < keyBits.size(); i++)
	{
		keyBits[i] = odd_parity[keyBits[i]];
	}

	return key.setKeyBits(keyBits);
}

size_t OSSLDES::getBlockSize() const
{
	// The block size is 64 bits
	return 64 >> 3;
}

