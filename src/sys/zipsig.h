#pragma once
// Whole-file signatures of zips (signapk -w): what Android's own recovery checks before it installs an OTA.
// FMRRE reads them the same way (install/verifier.cpp) and also says WHOSE key it is - the installed system's,
// one this recovery or the user trusts, the public AOSP test key, or an unknown one. A ROM zip signed with
// another key than the installed system's is a different ROM (or another signer): dirty-flashing it over the
// old one without a factory reset is one of the most common causes of a bootloop.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/x509.h"

namespace tw::zipsig {

enum class State { Unsigned, Bad, Valid, Error };
enum class Trust { Unknown, System, Recovery, User };

struct Result {
    State state = State::Unsigned;
    Trust trust = Trust::Unknown;
    bool testKey = false;          // the public AOSP test key: anyone can sign with it
    bool systemKeysKnown = false;  // the installed system's OTA keys could be read
    std::string reason;            // Unsigned / Bad / Error: what was found
    x509::Cert cert;               // Valid: the certificate whose key made the signature
    std::string hash;              // "SHA-256" / "SHA-1"
    std::string trustedBy;         // the key list that has this key
    std::string systemNote;        // why the installed system's keys weren't read

    bool valid() const { return state == State::Valid; }
    bool trusted() const { return valid() && trust != Trust::Unknown; }
    // Valid, the installed system's keys are known, and they don't include this one.
    bool otherThanSystem() const { return valid() && systemKeysKnown && trust != Trust::System; }
    std::string signer() const;    // a name for the signing key
    std::string headline() const;  // one sentence: "Signed with the installed system's key" ...
    std::string detail() const;    // signer, key, hash / what is wrong
    std::string brief() const;     // for lists and the flash history: "signed: LineageOS (installed system's key)"
    std::string badge() const;     // a word or two for a list row
};

struct Keys {
    std::vector<x509::Cert> system, recovery, user;
    std::string systemFrom, recoveryFrom, userFrom;
    std::string systemNote;  // why the installed system's keys couldn't be read
};

// Where the keys are. current() looks at the partitions - call it on the main thread; loadKeys() can then run
// anywhere (it mounts system read-only for a moment when it isn't mounted).
struct Sources {
    std::string systemRoot;           // the system partition's files, when they can be read now
    std::string systemDev, systemFs;  // else this is mounted read-only to read them
    std::string recoveryZip;          // this recovery's own otacerts.zip
    std::string userDir;              // FMRRE/keys: keys the user added
    static Sources current();
};
Keys loadKeys(const Sources& s);

using Progress = std::function<bool(uint64_t done, uint64_t total)>;  // false = cancel
Result verify(const std::string& zip, const Keys& keys, const Progress& progress = {});
// The verdict as log lines: what it is, who signed it with which key, which list trusts it.
std::vector<std::string> report(const Result& r);

// The same, in the background, for the screens: one check at a time, the result kept per file (path, size,
// modification time). check() starts it when needed; call it every frame.
struct Check {
    bool done = false;
    float progress = 0;
    Result result;
};
Check check(const std::string& zip);
bool cached(const std::string& zip, Result& r);  // a finished check of this file, unchanged since

// Save the signing certificate to FMRRE/keys: its signatures count as trusted from then on.
bool trust(const Result& r, std::string& err, std::string* savedTo = nullptr);

}  // namespace tw::zipsig
