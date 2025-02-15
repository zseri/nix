#include <iostream>
#include <cstring>

#include <openssl/crypto.h>
#include <openssl/md5.h>
#include <openssl/sha.h>

#include "args.hh"
#include "hash.hh"
#include "archive.hh"
#include "split.hh"
#include "util.hh"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace nix {

static size_t regularHashSize(HashType type) {
    switch (type) {
    case htMD5: return md5HashSize;
    case htSHA1: return sha1HashSize;
    case htSHA256: return sha256HashSize;
    case htSHA512: return sha512HashSize;
    }
    abort();
}


std::set<std::string> hashTypes = { "md5", "sha1", "sha256", "sha512" };


Hash::Hash(HashType type) : type(type)
{
    hashSize = regularHashSize(type);
    assert(hashSize <= maxHashSize);
    memset(hash, 0, maxHashSize);
}


bool Hash::operator == (const Hash & h2) const
{
    if (hashSize != h2.hashSize) return false;
    for (unsigned int i = 0; i < hashSize; i++)
        if (hash[i] != h2.hash[i]) return false;
    return true;
}


bool Hash::operator != (const Hash & h2) const
{
    return !(*this == h2);
}


bool Hash::operator < (const Hash & h) const
{
    if (hashSize < h.hashSize) return true;
    if (hashSize > h.hashSize) return false;
    for (unsigned int i = 0; i < hashSize; i++) {
        if (hash[i] < h.hash[i]) return true;
        if (hash[i] > h.hash[i]) return false;
    }
    return false;
}


const std::string base16Chars = "0123456789abcdef";


static std::string printHash16(const Hash & hash)
{
    std::string buf;
    buf.reserve(hash.hashSize * 2);
    for (unsigned int i = 0; i < hash.hashSize; i++) {
        buf.push_back(base16Chars[hash.hash[i] >> 4]);
        buf.push_back(base16Chars[hash.hash[i] & 0x0f]);
    }
    return buf;
}


// omitted: E O U T
const std::string base32Chars = "0123456789abcdfghijklmnpqrsvwxyz";


static std::string printHash32(const Hash & hash)
{
    assert(hash.hashSize);
    size_t len = hash.base32Len();
    assert(len);

    std::string s;
    s.reserve(len);

    for (int n = (int) len - 1; n >= 0; n--) {
        unsigned int b = n * 5;
        unsigned int i = b / 8;
        unsigned int j = b % 8;
        unsigned char c =
            (hash.hash[i] >> j)
            | (i >= hash.hashSize - 1 ? 0 : hash.hash[i + 1] << (8 - j));
        s.push_back(base32Chars[c & 0x1f]);
    }

    return s;
}


std::string printHash16or32(const Hash & hash)
{
    assert(hash.type);
    return hash.to_string(hash.type == htMD5 ? HashFormat::Base16 : HashFormat::Base32, false);
}


std::string Hash::to_string(HashFormat hashFormat, bool includeType) const
{
    std::string s;
    if (hashFormat == HashFormat::SRI || includeType) {
        s += printHashType(type);
        s += hashFormat == HashFormat::SRI ? '-' : ':';
    }
    switch (hashFormat) {
    case HashFormat::Base16:
        s += printHash16(*this);
        break;
    case HashFormat::Base32:
        s += printHash32(*this);
        break;
    case HashFormat::Base64:
    case HashFormat::SRI:
        s += base64Encode(std::string_view((const char *) hash, hashSize));
        break;
    }
    return s;
}

Hash Hash::dummy(htSHA256);

Hash Hash::parseSRI(std::string_view original) {
    auto rest = original;

    // Parse the has type before the separater, if there was one.
    auto hashRaw = splitPrefixTo(rest, '-');
    if (!hashRaw)
        throw BadHash("hash '%s' is not SRI", original);
    HashType parsedType = parseHashType(*hashRaw);

    return Hash(rest, parsedType, true);
}

// Mutates the string to eliminate the prefixes when found
static std::pair<std::optional<HashType>, bool> getParsedTypeAndSRI(std::string_view & rest)
{
    bool isSRI = false;

    // Parse the hash type before the separator, if there was one.
    std::optional<HashType> optParsedType;
    {
        auto hashRaw = splitPrefixTo(rest, ':');

        if (!hashRaw) {
            hashRaw = splitPrefixTo(rest, '-');
            if (hashRaw)
                isSRI = true;
        }
        if (hashRaw)
            optParsedType = parseHashType(*hashRaw);
    }

    return {optParsedType, isSRI};
}

Hash Hash::parseAnyPrefixed(std::string_view original)
{
    auto rest = original;
    auto [optParsedType, isSRI] = getParsedTypeAndSRI(rest);

    // Either the string or user must provide the type, if they both do they
    // must agree.
    if (!optParsedType)
        throw BadHash("hash '%s' does not include a type", rest);

    return Hash(rest, *optParsedType, isSRI);
}

Hash Hash::parseAny(std::string_view original, std::optional<HashType> optType)
{
    auto rest = original;
    auto [optParsedType, isSRI] = getParsedTypeAndSRI(rest);

    // Either the string or user must provide the type, if they both do they
    // must agree.
    if (!optParsedType && !optType)
        throw BadHash("hash '%s' does not include a type, nor is the type otherwise known from context", rest);
    else if (optParsedType && optType && *optParsedType != *optType)
        throw BadHash("hash '%s' should have type '%s'", original, printHashType(*optType));

    HashType hashType = optParsedType ? *optParsedType : *optType;
    return Hash(rest, hashType, isSRI);
}

Hash Hash::parseNonSRIUnprefixed(std::string_view s, HashType type)
{
    return Hash(s, type, false);
}

Hash::Hash(std::string_view rest, HashType type, bool isSRI)
    : Hash(type)
{
    if (!isSRI && rest.size() == base16Len()) {

        auto parseHexDigit = [&](char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            throw BadHash("invalid base-16 hash '%s'", rest);
        };

        for (unsigned int i = 0; i < hashSize; i++) {
            hash[i] =
                parseHexDigit(rest[i * 2]) << 4
                | parseHexDigit(rest[i * 2 + 1]);
        }
    }

    else if (!isSRI && rest.size() == base32Len()) {

        for (unsigned int n = 0; n < rest.size(); ++n) {
            char c = rest[rest.size() - n - 1];
            unsigned char digit;
            for (digit = 0; digit < base32Chars.size(); ++digit) /* !!! slow */
                if (base32Chars[digit] == c) break;
            if (digit >= 32)
                throw BadHash("invalid base-32 hash '%s'", rest);
            unsigned int b = n * 5;
            unsigned int i = b / 8;
            unsigned int j = b % 8;
            hash[i] |= digit << j;

            if (i < hashSize - 1) {
                hash[i + 1] |= digit >> (8 - j);
            } else {
                if (digit >> (8 - j))
                    throw BadHash("invalid base-32 hash '%s'", rest);
            }
        }
    }

    else if (isSRI || rest.size() == base64Len()) {
        auto d = base64Decode(rest);
        if (d.size() != hashSize)
            throw BadHash("invalid %s hash '%s'", isSRI ? "SRI" : "base-64", rest);
        assert(hashSize);
        memcpy(hash, d.data(), hashSize);
    }

    else
        throw BadHash("hash '%s' has wrong length for hash type '%s'", rest, printHashType(this->type));
}

Hash newHashAllowEmpty(std::string_view hashStr, std::optional<HashType> ht)
{
    if (hashStr.empty()) {
        if (!ht)
            throw BadHash("empty hash requires explicit hash type");
        Hash h(*ht);
        warn("found empty hash, assuming '%s'", h.to_string(HashFormat::SRI, true));
        return h;
    } else
        return Hash::parseAny(hashStr, ht);
}


union Ctx
{
    MD5_CTX md5;
    SHA_CTX sha1;
    SHA256_CTX sha256;
    SHA512_CTX sha512;
};


static void start(HashType ht, Ctx & ctx)
{
    if (ht == htMD5) MD5_Init(&ctx.md5);
    else if (ht == htSHA1) SHA1_Init(&ctx.sha1);
    else if (ht == htSHA256) SHA256_Init(&ctx.sha256);
    else if (ht == htSHA512) SHA512_Init(&ctx.sha512);
}


static void update(HashType ht, Ctx & ctx,
    std::string_view data)
{
    if (ht == htMD5) MD5_Update(&ctx.md5, data.data(), data.size());
    else if (ht == htSHA1) SHA1_Update(&ctx.sha1, data.data(), data.size());
    else if (ht == htSHA256) SHA256_Update(&ctx.sha256, data.data(), data.size());
    else if (ht == htSHA512) SHA512_Update(&ctx.sha512, data.data(), data.size());
}


static void finish(HashType ht, Ctx & ctx, unsigned char * hash)
{
    if (ht == htMD5) MD5_Final(hash, &ctx.md5);
    else if (ht == htSHA1) SHA1_Final(hash, &ctx.sha1);
    else if (ht == htSHA256) SHA256_Final(hash, &ctx.sha256);
    else if (ht == htSHA512) SHA512_Final(hash, &ctx.sha512);
}


Hash hashString(HashType ht, std::string_view s)
{
    Ctx ctx;
    Hash hash(ht);
    start(ht, ctx);
    update(ht, ctx, s);
    finish(ht, ctx, hash.hash);
    return hash;
}


Hash hashFile(HashType ht, const Path & path)
{
    HashSink sink(ht);
    readFile(path, sink);
    return sink.finish().first;
}


HashSink::HashSink(HashType ht) : ht(ht)
{
    ctx = new Ctx;
    bytes = 0;
    start(ht, *ctx);
}

HashSink::~HashSink()
{
    bufPos = 0;
    delete ctx;
}

void HashSink::writeUnbuffered(std::string_view data)
{
    bytes += data.size();
    update(ht, *ctx, data);
}

HashResult HashSink::finish()
{
    flush();
    Hash hash(ht);
    nix::finish(ht, *ctx, hash.hash);
    return HashResult(hash, bytes);
}

HashResult HashSink::currentHash()
{
    flush();
    Ctx ctx2 = *ctx;
    Hash hash(ht);
    nix::finish(ht, ctx2, hash.hash);
    return HashResult(hash, bytes);
}


HashResult hashPath(
    HashType ht, const Path & path, PathFilter & filter)
{
    HashSink sink(ht);
    dumpPath(path, sink, filter);
    return sink.finish();
}


Hash compressHash(const Hash & hash, unsigned int newSize)
{
    Hash h(hash.type);
    h.hashSize = newSize;
    for (unsigned int i = 0; i < hash.hashSize; ++i)
        h.hash[i % newSize] ^= hash.hash[i];
    return h;
}


std::optional<HashFormat> parseHashFormatOpt(std::string_view hashFormatName)
{
    if (hashFormatName == "base16") return HashFormat::Base16;
    if (hashFormatName == "base32") return HashFormat::Base32;
    if (hashFormatName == "base64") return HashFormat::Base64;
    if (hashFormatName == "sri") return HashFormat::SRI;
    return std::nullopt;
}

HashFormat parseHashFormat(std::string_view hashFormatName)
{
    auto opt_f = parseHashFormatOpt(hashFormatName);
    if (opt_f)
        return *opt_f;
    throw UsageError("unknown hash format '%1%', expect 'base16', 'base32', 'base64', or 'sri'", hashFormatName);
}

std::string_view printHashFormat(HashFormat HashFormat)
{
    switch (HashFormat) {
    case HashFormat::Base64:
        return "base64";
    case HashFormat::Base32:
        return "base32";
    case HashFormat::Base16:
        return "base16";
    case HashFormat::SRI:
        return "sri";
    default:
        // illegal hash base enum value internally, as opposed to external input
        // which should be validated with nice error message.
        assert(false);
    }
}

std::optional<HashType> parseHashTypeOpt(std::string_view s)
{
    if (s == "md5") return htMD5;
    if (s == "sha1") return htSHA1;
    if (s == "sha256") return htSHA256;
    if (s == "sha512") return htSHA512;
    return std::nullopt;
}

HashType parseHashType(std::string_view s)
{
    auto opt_h = parseHashTypeOpt(s);
    if (opt_h)
        return *opt_h;
    else
        throw UsageError("unknown hash algorithm '%1%', expect 'md5', 'sha1', 'sha256', or 'sha512'", s);
}

std::string_view printHashType(HashType ht)
{
    switch (ht) {
    case htMD5: return "md5";
    case htSHA1: return "sha1";
    case htSHA256: return "sha256";
    case htSHA512: return "sha512";
    default:
        // illegal hash type enum value internally, as opposed to external input
        // which should be validated with nice error message.
        assert(false);
    }
}

}
