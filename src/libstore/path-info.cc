#include "path-info.hh"
#include "store-api.hh"

namespace nix {

GENERATE_CMP_EXT(
    ,
    UnkeyedValidPathInfo,
    me->deriver,
    me->narHash,
    me->references,
    me->registrationTime,
    me->narSize,
    //me->id,
    me->ultimate,
    me->sigs,
    me->ca);

GENERATE_CMP_EXT(
    ,
    ValidPathInfo,
    me->path,
    static_cast<const UnkeyedValidPathInfo &>(*me));

std::string ValidPathInfo::fingerprint(const Store & store) const
{
    if (narSize == 0)
        throw Error("cannot calculate fingerprint of path '%s' because its size is not known",
            store.printStorePath(path));
    return
        "1;" + store.printStorePath(path) + ";"
        + narHash.to_string(HashFormat::Base32, true) + ";"
        + std::to_string(narSize) + ";"
        + concatStringsSep(",", store.printStorePathSet(references));
}


void ValidPathInfo::sign(const Store & store, const SecretKey & secretKey)
{
    sigs.insert(secretKey.signDetached(fingerprint(store)));
}

std::optional<ContentAddressWithReferences> ValidPathInfo::contentAddressWithReferences() const
{
    if (! ca)
        return std::nullopt;

    return std::visit(overloaded {
        [&](const TextIngestionMethod &) -> ContentAddressWithReferences {
            assert(references.count(path) == 0);
            return TextInfo {
                .hash = ca->hash,
                .references = references,
            };
        },
        [&](const FileIngestionMethod & m2) -> ContentAddressWithReferences {
            auto refs = references;
            bool hasSelfReference = false;
            if (refs.count(path)) {
                hasSelfReference = true;
                refs.erase(path);
            }
            return FixedOutputInfo {
                .method = m2,
                .hash = ca->hash,
                .references = {
                    .others = std::move(refs),
                    .self = hasSelfReference,
                },
            };
        },
    }, ca->method.raw);
}

bool ValidPathInfo::isContentAddressed(const Store & store) const
{
    auto fullCaOpt = contentAddressWithReferences();

    if (! fullCaOpt)
        return false;

    auto caPath = store.makeFixedOutputPathFromCA(path.name(), *fullCaOpt);

    bool res = caPath == path;

    if (!res)
        printError("warning: path '%s' claims to be content-addressed but isn't", store.printStorePath(path));

    return res;
}


size_t ValidPathInfo::checkSignatures(const Store & store, const PublicKeys & publicKeys) const
{
    if (isContentAddressed(store)) return maxSigs;

    size_t good = 0;
    for (auto & sig : sigs)
        if (checkSignature(store, publicKeys, sig))
            good++;
    return good;
}


bool ValidPathInfo::checkSignature(const Store & store, const PublicKeys & publicKeys, const std::string & sig) const
{
    return verifyDetached(fingerprint(store), sig, publicKeys);
}


Strings ValidPathInfo::shortRefs() const
{
    Strings refs;
    for (auto & r : references)
        refs.push_back(std::string(r.to_string()));
    return refs;
}

ValidPathInfo::ValidPathInfo(
    const Store & store,
    std::string_view name,
    ContentAddressWithReferences && ca,
    Hash narHash)
      : UnkeyedValidPathInfo(narHash)
      , path(store.makeFixedOutputPathFromCA(name, ca))
{
    std::visit(overloaded {
        [this](TextInfo && ti) {
            this->references = std::move(ti.references);
            this->ca = ContentAddress {
                .method = TextIngestionMethod {},
                .hash = std::move(ti.hash),
            };
        },
        [this](FixedOutputInfo && foi) {
            this->references = std::move(foi.references.others);
            if (foi.references.self)
                this->references.insert(path);
            this->ca = ContentAddress {
                .method = std::move(foi.method),
                .hash = std::move(foi.hash),
            };
        },
    }, std::move(ca).raw);
}

}
