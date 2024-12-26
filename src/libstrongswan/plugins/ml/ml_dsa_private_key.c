/*
 * Copyright (C) 2024 Andreas Steffen, strongSec GmbH
 *
 * Copyright (C) secunet Security Networks AG
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "ml_dsa_private_key.h"

#include <utils/debug.h>
#include <asn1/asn1.h>
#include <credentials/cred_encoding.h>
#include <credentials/keys/public_key.h>

typedef struct private_private_key_t private_private_key_t;

#define ML_DSA_SEED_LEN	32

/**
 * Private data
 */
struct private_private_key_t {

	/**
	 * Public interface
	 */
	private_key_t public;

	/**
	 * Key type
	 */
	key_type_t type;

	/**
	 * Secret key seed
	 */
	chunk_t keyseed;

	/**
	 * Public key
	 */
	chunk_t pubkey;

	/**
	 * Reference count
	 */
	refcount_t ref;
};

/**
 * Generate two types of ML-DSA fingerprints
 */
bool ml_dsa_fingerprint(chunk_t pubkey, key_type_t type,
						cred_encoding_type_t enc_type, chunk_t *fp)
{
	chunk_t encoding;
	hasher_t *hasher;

	*fp = chunk_empty;

	switch (enc_type)
	{
		case KEYID_PUBKEY_SHA1:
			encoding = chunk_clone(pubkey);
			break;
		case KEYID_PUBKEY_INFO_SHA1:
			encoding = public_key_info_encode(pubkey, key_type_to_oid(type));
			break;
		default:
			return FALSE;
	}
	chunk_free(&encoding);

	hasher = lib->crypto->create_hasher(lib->crypto, HASH_SHA1);
	if (!hasher || !hasher->allocate_hash(hasher, encoding, fp))
	{
		DBG1(DBG_LIB, "SHA1 hash algorithm not supported");
		DESTROY_IF(hasher);
		return FALSE;
	}
	hasher->destroy(hasher);

	return TRUE;
}

METHOD(private_key_t, sign, bool,
	private_private_key_t *this, signature_scheme_t scheme,
	void *params, chunk_t data, chunk_t *signature)
{
	if (key_type_from_signature_scheme(scheme) != this->type)
	{
		DBG1(DBG_LIB, "signature scheme %N not supported",
					   signature_scheme_names, scheme);
		return FALSE;
	}

	return FALSE;
}

METHOD(private_key_t, decrypt, bool,
	private_private_key_t *this, encryption_scheme_t scheme,
	void *params, chunk_t crypto, chunk_t *plain)
{
	DBG1(DBG_LIB, "ML-DSA private key decryption not implemented");
	return FALSE;
}

METHOD(private_key_t, get_keysize, int,
	private_private_key_t *this)
{
	return BITS_PER_BYTE * get_public_key_size(this->type);
}

METHOD(private_key_t, get_type, key_type_t,
	private_private_key_t *this)
{
	return this->type;
}

METHOD(private_key_t, get_public_key, public_key_t*,
	private_private_key_t *this)
{
	return lib->creds->create(lib->creds, CRED_PUBLIC_KEY, this->type,
							  BUILD_BLOB, this->pubkey, BUILD_END);
}

METHOD(private_key_t, get_fingerprint, bool,
	private_private_key_t *this, cred_encoding_type_t type,	chunk_t *fp)
{
	bool success;

	if (lib->encoding->get_cache(lib->encoding, type, this, fp))
	{
		return TRUE;
	}

	success = ml_dsa_fingerprint(this->pubkey, this->type, type, fp);
	if (success)
	{
		lib->encoding->cache(lib->encoding, type, this, fp);
	}

	return success;
}

METHOD(private_key_t, get_encoding, bool,
	private_private_key_t *this, cred_encoding_type_t type, chunk_t *encoding)
{
	switch (type)
	{
		case PRIVKEY_ASN1_DER:
		case PRIVKEY_PEM:
		{
			bool success = TRUE;
			int oid = key_type_to_oid(this->type);

			*encoding = asn1_wrap(ASN1_SEQUENCE, "cmm",
							ASN1_INTEGER_0,
							asn1_algorithmIdentifier(oid),
							asn1_simple_object(ASN1_OCTET_STRING, this->keyseed)
						);
			if (type == PRIVKEY_PEM)
			{
				chunk_t asn1_encoding = *encoding;

				success = lib->encoding->encode(lib->encoding, PRIVKEY_PEM,
								NULL, encoding, CRED_PART_PRIV_ASN1_DER,
								asn1_encoding, CRED_PART_END);
				chunk_clear(&asn1_encoding);
			}

			return success;
		}
		default:
			return FALSE;
	}
}

METHOD(private_key_t, get_ref, private_key_t*,
	private_private_key_t *this)
{
	ref_get(&this->ref);
	return &this->public;
}

METHOD(private_key_t, destroy, void,
	private_private_key_t *this)
{
	if (ref_put(&this->ref))
	{
		lib->encoding->clear_cache(lib->encoding, this);
		chunk_clear(&this->keyseed);
		chunk_free(&this->pubkey);
		free(this);
	}
}

/**
 * Generic private constructor
 */
static private_private_key_t *create_instance(key_type_t type, chunk_t keyseed)
{
	private_private_key_t *this;

	INIT(this,
		.public = {
			.get_type = _get_type,
			.sign = _sign,
			.decrypt = _decrypt,
			.get_keysize = _get_keysize,
			.get_public_key = _get_public_key,
			.equals = private_key_equals,
			.belongs_to = private_key_belongs_to,
			.get_fingerprint = _get_fingerprint,
			.has_fingerprint = private_key_has_fingerprint,
			.get_encoding = _get_encoding,
			.get_ref = _get_ref,
			.destroy = _destroy,
		},
		.type = type,
		.keyseed = keyseed,
		.ref = 1,
	);

	return this;
}

/*
 * Described in header
 */
private_key_t *ml_dsa_private_key_gen(key_type_t type, va_list args)
{
	private_private_key_t *this;
	chunk_t seed;

	while (TRUE)
	{
		switch (va_arg(args, builder_part_t))
		{
			case BUILD_KEY_SIZE:
				/* just ignore the key size */
				va_arg(args, u_int);
				continue;
			case BUILD_END:
				break;
			default:
				return NULL;
		}
		break;
	}

	seed = chunk_alloc(ML_DSA_SEED_LEN);

	this = create_instance(type, seed);
	if (!this)
	{
		return NULL;
	}

	return &this->public;
}

/*
 * Described in header
 */
private_key_t *ml_dsa_private_key_load(key_type_t type, va_list args)
{
	private_private_key_t *this;
	chunk_t priv = chunk_empty;

	while (TRUE)
	{
		switch (va_arg(args, builder_part_t))
		{
			case BUILD_PRIV_ASN1_DER:
				priv = va_arg(args, chunk_t);
				continue;
			case BUILD_END:
				break;
			default:
				return NULL;
		}
		break;
	}

	if (priv.len == 0)
	{
		return NULL;
	}
	if (priv.len != ML_DSA_SEED_LEN)
	{
		DBG1(DBG_LIB, "the size of the loaded ML-DSA private key seed"
					  "is not %d bytes", ML_DSA_SEED_LEN);
		return NULL;
	}

	this = create_instance(type, chunk_clone(priv));
	if (!this)
	{
		return NULL;
	}

	return &this->public;
}