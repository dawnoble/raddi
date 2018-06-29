#include "raddi_entry.h"
#include "raddi_channel.h"
#include "raddi_identity.h"
#include "raddi_consensus.h"

#include "../common/log.h"

#include <windows.h>
#include <cstring>
#include <vector>

// NOTE: validate/verify strings are in raddi_database.rc, "DATABASE | DATA | 0x1?" rows

bool raddi::entry::validate (const void * header, std::size_t length) {
    if (length < sizeof (entry) + proof::min_size)
        return false;

    auto e = static_cast <const entry *> (header);
    auto now = raddi::now ();

    if (raddi::older (e->id.timestamp, e->parent.timestamp)) {
        return raddi::log::data (raddi::component::database, 0x10, e->id, e->id.timestamp, e->parent.timestamp);
    }
    if (raddi::older (e->id.timestamp, e->id.identity.timestamp)) {
        return raddi::log::data (raddi::component::database, 0x11, e->id, e->id.timestamp, e->id.identity.timestamp);
    }
    if (raddi::older (e->parent.timestamp, e->parent.identity.timestamp)) {
        return raddi::log::data (raddi::component::database, 0x12, e->id, e->parent.timestamp, e->parent.identity.timestamp);
    }
    if (raddi::older (e->id.timestamp, now - 0x40000000u)) {
        return raddi::log::data (raddi::component::database, 0x13, e->id, e->id.timestamp, now - 0x40000000u, 0x40000000u / (60 * 60 * 24 * 365));
    }
    if (raddi::older (now + raddi::consensus::max_entry_skew_allowed, e->id.timestamp)) {
        return raddi::log::data (raddi::component::database, 0x14, e->id, now + raddi::consensus::max_entry_skew_allowed,
                                 e->id.timestamp, raddi::consensus::max_entry_skew_allowed);
    }

    if (auto channel = e->is_announcement ()) {
        switch (channel) {
            case new_identity_announcement:
                return length >= sizeof (raddi::identity)
                    || raddi::log::data (raddi::component::database, 0x18, e->id, length, sizeof (raddi::identity));
            case new_channel_announcement:
                return length >= sizeof (raddi::channel)
                    || raddi::log::data (raddi::component::database, 0x19, e->id, length, sizeof (raddi::channel));
            default:
                return false; // unreachable
        }
    } else
        return length >= sizeof (raddi::entry) + 1
            || raddi::log::data (raddi::component::database, 0x1A, e->id);
}

const raddi::proof * raddi::entry::proof (std::size_t size, std::size_t * proof_size) const {
    size -= sizeof (entry);

    for (auto length = proof::min_length; length != proof::max_length + 2; length += 2) { // inclusive iteration
        auto n = proof::size (length);
        if (n <= size) {
            auto p = this->content () + size - n;

            if (raddi::proof::validate (p, n)) { // tests for NUL byte so we don't need to search explicitly here
                if (proof_size) {
                    *proof_size = n;
                }
                return reinterpret_cast <const raddi::proof *> (p);
            }
        } else
            break;
    }
    return nullptr;
}

crypto_sign_ed25519ph_state raddi::entry::prehash (std::size_t size, const entry * parent, std::size_t parent_size) const {
    crypto_sign_ed25519ph_state state;
    crypto_sign_ed25519ph_init (&state);
    crypto_sign_ed25519ph_update (&state, reinterpret_cast <const unsigned char *> (parent), parent_size);
    crypto_sign_ed25519ph_update (&state, reinterpret_cast <const unsigned char *> (&this->id), sizeof this->id);
    crypto_sign_ed25519ph_update (&state, reinterpret_cast <const unsigned char *> (&this->parent), sizeof this->parent);
    crypto_sign_ed25519ph_update (&state, reinterpret_cast <const unsigned char *> (this->content ()), size - sizeof (entry));
    return state;
}

bool raddi::entry::verify (std::size_t size, const entry * parent, std::size_t parent_size,
                           const std::uint8_t (&public_key) [crypto_sign_ed25519_PUBLICKEYBYTES]) const {
    std::size_t proof_size;
    
    if (auto proof = this->proof (size, &proof_size)) {
        auto imprint = this->prehash (size - proof_size, parent, parent_size);

        if (proof->verify (imprint.hs)) {
            crypto_sign_ed25519ph_update (&imprint, reinterpret_cast <const unsigned char *> (proof), proof_size);
            return crypto_sign_ed25519ph_final_verify (&imprint, const_cast <std::uint8_t *> (this->signature), public_key) == 0
                || raddi::log::data (raddi::component::database, 0x1E, this->id, size);
        }
    }
    return raddi::log::data (raddi::component::database, 0x1F, this->id, size);
}

std::size_t raddi::entry::sign (std::size_t size, const entry * parent, std::size_t parent_size,
                                const std::uint8_t (&private_key) [crypto_sign_ed25519_SECRETKEYBYTES],
                                proof::requirements rq, volatile bool * cancel) {
    if (size >= sizeof (entry)) {

        auto imprint = this->prehash (size, parent, parent_size);
        auto proof_ptr = this->content () + size;

        if (auto proof_size = raddi::proof::generate (imprint.hs,
                                                      proof_ptr, entry::max_content_size - size,
                                                      rq, cancel)) {
            size += proof_size;
            crypto_sign_ed25519ph_update (&imprint, proof_ptr, proof_size);

            if (crypto_sign_ed25519ph_final_create (&imprint, this->signature, nullptr, private_key) == 0)
                return proof_size;
        }
    }
    return 0;
}

std::size_t raddi::entry::sign (std::size_t size, const entry * parent, std::size_t parent_size,
                                const std::uint8_t (&private_key) [crypto_sign_ed25519_SECRETKEYBYTES],
                                volatile bool * cancel) {
    return this->sign (size, parent, parent_size, private_key,
                       this->default_requirements (), cancel);
}

raddi::entry::announcement_type raddi::entry::is_announcement () const {
    return entry::is_announcement (this->id, this->parent);
}

raddi::entry::announcement_type raddi::entry::is_announcement (const raddi::eid & id, const raddi::eid & parent) {
    if (id == parent) {
        if (id.timestamp == id.identity.timestamp) {
            return new_identity_announcement;
        } else {
            return new_channel_announcement;
        }
    } else
        return not_an_announcement;
}

raddi::proof::requirements raddi::entry::default_requirements () const {
    return proof::requirements ();
    /*
    // TODO: export contants to raddi_consensus.h
    //  - perhaps make it a block of parameters that's also passed to propose, accept, sign and validate?

    switch (this->is_announcement ()) {
        case new_identity_announcement:
            return { 22, (1 << eid::type_bit_depth) * 1000 };

        case new_channel_announcement:
            return { 21, (1 << eid::type_bit_depth) * 1000 / 2 };

        case not_an_announcement:
        default:
            switch (this->id.classify ()) {

                case eid::type::upvote:
                    return { proof::min_complexity, 200 };

                case eid::type::downvote:
                default:
                    return proof::requirements ();
            }
    }*/
}
