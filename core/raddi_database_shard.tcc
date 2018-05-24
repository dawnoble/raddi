#ifndef RADDI_DATABASE_SHARD_TCC
#define RADDI_DATABASE_SHARD_TCC

template <typename Key>
std::wstring raddi::db::shard <Key>::path (const db::table <Key> * table, const wchar_t * suffix) const {
    wchar_t filename [32768];
    _snwprintf (filename, (sizeof filename / sizeof filename [0]) - 256,
                L"%s\\%s\\%08x%s",
                table->db.path.c_str (), table->name.c_str (), this->base, suffix);
    return filename;
}

template <typename Key>
raddi::db::shard <Key>::shard (std::uint32_t base, const raddi::db::table <Key> * parent)
    : provider ("shard", parent ? parent->db.shard_instance_name (base, parent->name) : L"\x0018") // u0018 is moved-from or temporary object
    , base (base)
    , accessed (raddi::now ()) {
}

template <typename Key>
raddi::db::shard <Key>::shard (raddi::db::shard <Key> && other)
    : provider (std::move (other))
    , base (other.base)
    , accessed (raddi::now ())
    , index (std::move (other.index))
    , content (std::move (other.content))
    , cache (std::move (other.cache)) {}

template <typename Key>
raddi::db::shard <Key> & raddi::db::shard <Key>::operator = (raddi::db::shard <Key> && other) {
    this->unsynchronized_close ();

    exclusive guard2 (other.lock);

    this->base = other.base;
    this->accessed = other.accessed;
    this->index = std::move (other.index);
    this->content = std::move (other.content);
    this->cache.swap (other.cache);
    return *this;
}

template <typename Key>
void raddi::db::shard <Key>::unsynchronized_close () {
    this->cache.clear ();
    this->index.close ();
    this->content.close ();
}

template <typename Key>
bool raddi::db::shard <Key>::close () {
    if (!this->closed ()) {
        exclusive guard (this->lock);
        this->unsynchronized_close ();
        this->cache.shrink_to_fit ();
        return true;
    } else
        return false;
}

template <typename Key>
void raddi::db::shard <Key>::flush () {
    this->content.flush ();
    this->index.flush ();
}

template <typename Key>
bool raddi::db::shard <Key>::advance (const db::table <Key> * table) {
    exclusive guard (this->lock);
    return this->unsynchronized_advance (table);
}

template <typename Key>
bool raddi::db::shard <Key>::reload (const db::table <Key> * table) {
    exclusive lock (this->lock);
    this->unsynchronized_close ();
    return this->unsynchronized_advance (table);
}

template <typename Key>
std::size_t raddi::db::shard <Key>::size (const db::table <Key> * table) const {
    immutability guard (this->lock);
    if (this->index.closed ()) {

        file f;
        if (f.open (this->path (table), file::mode::open, file::access::query, file::share::full, file::buffer::none)) {
            auto n = f.size () / sizeof (Key);
#ifndef _WIN64
            if (n > this->cache.max_size () / (table->db.settings.maximum_active_shards ? table->db.settings.maximum_active_shards : 1)) {
                log::error (component::database, 23,
                            4096 / table->db.settings.minimum_active_shards,
                            (n * sizeof (Key)) / (1024 * 1024));
                return static_cast <std::size_t> (-1);
            }
#endif
            return (std::size_t) n;
        } else {
            this->report (log::level::error, 11, this->path (table), file::mode::open, file::share::full);
            return 0;
        }
    } else
        return this->cache.size ();
}

template <typename Key>
bool raddi::db::shard <Key>::unsynchronized_advance (const db::table <Key> * table) {
    file::mode open;
    file::share share;
    bool opened = false;

    if (!table)
        return true;

    switch (table->db.mode) {
        case file::access::read:
            open = file::mode::open;
            share = file::share::full;
            break;

        case file::access::write:
            // there can be only single writer, file already loaded
            if (!this->index.closed ())
                return true;

            open = file::mode::always;
            share = file::share::full; // TODO: 'split' requires share::full
            break;
    }

    if (this->index.closed ()) {
        if (this->index.open (this->path (table), open, table->db.mode, share, file::buffer::sequential)) {
            if ((open == file::mode::always) && this->index.created ()) {
                this->report (log::level::event, 12, this->path (table));
            } else {
                this->report (log::level::note, 12, this->path (table));
            }
            opened = true;
        } else {
            this->report (log::level::error, 11, this->path (table), table->db.mode, share);
            return false;
        }
    }

    if (this->content.closed ()) {
        if (!this->content.open (this->path (table, L"d"), open, table->db.mode, share, file::buffer::random)) {
            this->report (log::level::error, 12, this->path (table, L"d"), table->db.mode, share);
            this->index.close ();
            return false;
        }
    }

    try {
        const auto size = this->index.size ();
        const auto n = size / sizeof (Key);

        if (n == this->cache.size ())
            return true;

        if (n) {

#ifndef _WIN64
            if (n > this->cache.max_size () / table->db.settings.minimum_active_shards) {
                log::error (component::database, 23,
                            4096 / table->db.settings.minimum_active_shards,
                            (n * sizeof (Key)) / (1024 * 1024));
                throw std::bad_alloc ();
            }
            // shadow 'n' to suppress integer type conversion warnings since the condition above makes sure it will always fit
            std::size_t n ((std::size_t) n);
#endif
            if (opened && (table->db.mode == file::access::write)) {
                // reserve some extra space, a little for old shards, more for recent

                auto age = (raddi::now () - this->base) / table->db.settings.forward_granularity;
                std::size_t reserve = table->db.settings.maximum_shard_size;
                while (age /= 2) {
                    reserve /= 2;
                }
                this->cache.reserve (std::max (reserve, 4096 / sizeof (Key))); // 16384?
            }

            if (opened) { // TODO: measure if we need this path at all since this is likely i/o bound anyway
                this->cache.resize (n);

                if (this->index.read (&this->cache [0], n * sizeof (Key))) {
                    std::sort (this->cache.begin (), this->cache.end ());
                    this->cache.erase (this->cache.begin (),
                                       std::find_if_not (this->cache.begin (), this->cache.end (),
                                                         [] (auto & x) { return x.id.erased (); }));
                }
            } else {
                this->cache.reserve (n);

                Key row;
                std::memset (&row, 0, sizeof row);

                while (this->index.read (row)) {
                    if (!row.id.erased ()) {
                        this->unsynchronized_insert_to_cache (row);
                    }
                }
            }
            this->report (log::level::note, 13, this->cache.size ());
        }
        return true;
    } catch (const std::bad_alloc &) {
        this->report (log::level::error, 13);
    }
    return false;
}

template <typename Key>
void raddi::db::shard <Key>::unsynchronized_insert_to_cache (const Key & r) {
    if (this->cache.empty () || (this->cache.back ().id < r.id)) {
        this->cache.push_back (r);
    } else {
        this->cache.insert (std::lower_bound (this->cache.begin (), this->cache.end (), r), r);
    }
}

template <typename Key>
bool raddi::db::shard <Key>::insert (const db::table <Key> * table, const void * data, std::size_t size, const root & top, bool & exists) {
    auto entry = static_cast <const raddi::entry *> (data);

    exclusive guard (this->lock);

    if (!table->db.settings.reinsertion_validation) {
        exists = this->unsynchronized_get ((const decltype (Key::id)) entry->id);

    } else {
        std::uint8_t buffer [sizeof (raddi::entry) + raddi::entry::max_content_size];
        std::size_t length;

        exists = this->unsynchronized_get ((const decltype (Key::id)) entry->id, nullptr, read::everything, buffer, &length);

        if (exists) {
            if (size != length) {
                return this->report (log::level::data, 7, entry->id.serialize (), size, length);
            }
            if (std::memcmp (buffer, data, size) != 0) {
                return this->report (log::level::data, 7, entry->id.serialize (), size, length);
            }
        }
    }

    return exists
        || this->unsynchronized_insert (table, data, size, top);
}

template <typename Key>
bool raddi::db::shard <Key>::unsynchronized_insert (const db::table <Key> * table, const void * data, std::size_t size, const root & top) {
    if (size >= sizeof (raddi::entry)
     && size <= sizeof (raddi::entry) + raddi::entry::max_content_size) {

        if (this->unsynchronized_advance (table)) {

            // TODO: use file locking to protect readers against shard splits?

            const auto prefix = sizeof (raddi::entry::id) + sizeof (raddi::entry::parent);
            const auto iposition = this->index.tell ();
            const auto cposition = this->content.tell ();

            if (!this->content.write (reinterpret_cast <const char *> (data) + prefix, size - prefix)) {
                this->content.resize (cposition);
                return this->report (log::level::error, 15, this->path (table));
            }

            Key row;
            if (row.classify (data, size, top)) {
                row.data.offset = cposition;
                row.data.length = size - sizeof (raddi::entry);

                if (!this->index.write (row)) {
                    this->index.resize (iposition);
                    this->content.resize (cposition);
                    return this->report (log::level::error, 14, this->path (table));
                }

                try {
                    this->unsynchronized_insert_to_cache (row);
                    this->accessed = raddi::now ();

                    this->report (log::level::note, 14, this->path (table), row.id);
                    return true;
                } catch (const std::bad_alloc &) {
                    this->report (log::level::error, 16, this->path (table));
                    this->unsynchronized_close ();
                    this->cache.shrink_to_fit (); // bad_alloc???
                }
            } else {
                this->report (log::level::error, 24, this->path (table), row.id, size);
            }
        }
    }
    return false;
}

template <typename Key>
bool raddi::db::shard <Key>::erase (const db::table <Key> * table, const decltype (Key::id) & id, bool thorough) {
    exclusive guard (this->lock);

    if (this->unsynchronized_advance (table)) {
        auto ie = this->cache.end ();
        auto ii = std::lower_bound (this->cache.begin (), ie, Key { id });
        if ((ii != ie) && (ii->id == id)) {

            if (thorough) {
                const auto length = ii->data.length + sizeof (raddi::entry::signature) + sizeof (raddi::entry::proof);
                if (!this->content.zero (ii->data.offset, length)) {
                    this->report (log::level::error, 19, this->path (table, L"d"), ii->data.offset, length);
                }
            }

            raddi::eid i;
            std::uintmax_t offset = 0;

            while (this->index.read (offset, i)) {
                if (!i.erased () && (i == id)) {
                    if (!this->index.zero (offset, sizeof (Key))) {
                        return this->report (log::level::error, 20, this->path (table), offset, sizeof (Key));
                    } else
                        break;
                }
                offset += sizeof (Key);
            }

            this->cache.erase (ii);
            return true;
        }
    }
    return false;
}

template <typename Key>
bool raddi::db::shard <Key>::get (const decltype (Key::id) & e, Key * r) {
    immutability guard (this->lock);
    return this->unsynchronized_get (e, r);
}

template <typename Key>
bool raddi::db::shard <Key>::get (const decltype (Key::id) & e, read what, void * buffer, std::size_t * length, std::size_t demand) {
    immutability guard (this->lock);
    return this->unsynchronized_get (e, nullptr, what, buffer, length, demand);
}

template <typename Key>
bool raddi::db::shard <Key>::unsynchronized_get (const decltype (Key::id) & id, Key * row,
                                                 read what, void * entry, std::size_t * size, std::size_t demand) {
    auto ie = this->cache.end ();
    auto ii = std::lower_bound (this->cache.begin (), ie, Key { id });
    if ((ii != ie) && (ii->id == id)) {

        if (row) {
            *row = *ii;
        }
        if (size) {
            *size = (std::size_t) ii->data.length + sizeof (raddi::entry);
        }

        this->accessed = raddi::now ();
        return this->unsynchronized_read (ii, what, entry, demand);
    } else
        return false;
}

template <typename Key>
bool raddi::db::shard <Key>::unsynchronized_read (typename std::vector <Key>::const_iterator ii,
                                                  read what, void * entry, std::size_t demand) {
    if ((what != read::nothing) && (entry != nullptr)) {
        switch (what) {
            case read::identification:
            case read::identification_and_verification:
            case read::identification_and_content:
            case read::everything:
                *reinterpret_cast <raddi::entry *> (entry) = (raddi::entry) *ii;
                break;
        }

        if (demand > ii->data.length) {
            this->unsynchronized_close ();
            return false; // TODO: report? unable to serve demanded amount of content, internal error or db corrupted
        }
        if (demand == 0) {
            demand = ii->data.length;
        }

        std::size_t length = 0;
        std::size_t offset = 0;

        switch (what) {
            case read::verification:
            case read::identification_and_verification:
                length = sizeof (raddi::entry::signature) + sizeof (raddi::entry::proof);
                break;
            case read::content:
            case read::identification_and_content:
                offset = sizeof (raddi::entry::signature) + sizeof (raddi::entry::proof);
                length = demand;
                break;
            case read::verification_and_content:
            case read::everything:
                length = sizeof (raddi::entry::signature) + sizeof (raddi::entry::proof)
                       + demand;
                break;

            default:
                return true;
        }

        auto target = reinterpret_cast <std::uint8_t *> (entry)
                    + sizeof (raddi::entry::id) + sizeof (raddi::entry::parent)
                    + offset;

        if (!this->content.read (ii->data.offset + offset, target, length)) {
            this->report (log::level::error, 17, ii->data.offset + offset, length);
            this->unsynchronized_close (); // corrupted db, force reload
            return false;
        }
    }
    return true;
}

template <typename Key>
raddi::db::shard <Key> raddi::db::shard <Key>::split (const db::table <Key> * table, std::uint32_t timestamp) {
    exclusive guard (this->lock);
    this->report (log::level::note, 15, timestamp);

    const auto suffix = L"d~" + std::to_wstring (raddi::microtimestamp ());
    const auto tmp_index_filename = this->path (table, suffix.c_str () + 1);
    const auto tmp_content_filename = this->path (table, suffix.c_str () + 0);

    if (this->unsynchronized_advance (table)
        && MoveFileEx (this->path (table).c_str (), tmp_index_filename.c_str (), MOVEFILE_REPLACE_EXISTING)
        && MoveFileEx (this->path (table, L"d").c_str (), tmp_content_filename.c_str (), MOVEFILE_REPLACE_EXISTING)) {

        // TODO: this and remaining probably use the same data file

        shard remaining (this->base, table);
        shard separated (timestamp, table);

        remaining.cache.reserve (this->cache.size ());
        separated.cache.reserve (this->cache.size ());

        std::size_t n1 = 0;
        std::size_t n2 = 0;

        // TODO: 'unsynchronized_enumerate'?
        for (const auto & row : this->cache) {

            std::size_t size;
            struct : public raddi::entry {
                std::uint8_t description [raddi::entry::max_content_size];
            } data;

            if (this->unsynchronized_get (row.id, nullptr, read::everything, &data, &size)) {
                if (!row.id.erased ()) {
                    if (row.id.timestamp >= timestamp) {
                        if (separated.unsynchronized_insert (table, &data, size, row.top ())) {
                            ++n1;
                        }
                    } else {
                        if (remaining.unsynchronized_insert (table, &data, size, row.top ())) {
                            ++n2;
                        }
                    }
                }
            }
        }

        this->report (log::level::event, 13, this->base, timestamp, n1, n2, this->cache.size () - (n1 + n2));
        *this = std::move (remaining);

        // assert (this->cache.size () == n2);

        DeleteFile (tmp_index_filename.c_str ());
        DeleteFile (tmp_content_filename.c_str ());

        return std::move (separated);
    } else {
        this->report (log::level::error, 18);
        throw (int) GetLastError ();
    }
}

template <typename Key>
    template <typename F>
void raddi::db::shard <Key> ::enumerate (F callback) {
    immutability guard (this->lock);

    auto i = this->cache.cbegin ();
    auto e = this->cache.cend ();

    for (; i != e; ++i) {
        if (callback (*i, nullptr)) {
            std::uint8_t data [raddi::protocol::max_payload];
            if (this->unsynchronized_read (i, read::everything, data)) {
                callback (*i, data);
            }
        }
    }
    this->accessed = raddi::now ();
}

#endif