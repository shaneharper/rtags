#ifndef CursorInfo_h
#define CursorInfo_h

#include <rct/String.h>
#include "Location.h"
#include <rct/Path.h>
#include <rct/Log.h>
#include <rct/List.h>
#include <clang-c/Index.h>
#include <assert.h>

class CursorInfo;
typedef Map<Location, CursorInfo> SymbolMap;
struct CursorData {
    CursorData()
        : symbolLength(0), kind(0), type(CXType_Invalid),
          definition(false), start(-1), end(-1), refCount(1)
    {}

    CursorData *detach()
    {
        if (__sync_add_and_fetch(&refCount, 0) != 1) {
            CursorData *ret = new CursorData;
            ret->symbolLength = symbolLength;
            ret->symbolName = symbolName;
            ret->kind = kind;
            ret->type = type;
            ret->enumValue = enumValue;
            ret->targets = targets;
            ret->references = references;
            ret->start = start;
            ret->end = end;
            return ret;
        }
        return this;
    }

    void ref()
    {
        __sync_add_and_fetch(&refCount, 1);
    }

    void deref()
    {
        if (!__sync_add_and_fetch(&refCount, -1)) {
            delete this;
        }
    }

    uint16_t symbolLength; // this is just the symbol name length e.g. foo => 3
    String symbolName; // this is fully qualified Foobar::Barfoo::foo
    uint16_t kind;
    CXTypeKind type;
    union {
        bool definition;
        int64_t enumValue; // only used if kind == CXCursor_EnumConstantDecl
    };
    Set<Location> targets, references;
    int start, end;
    mutable volatile int refCount;
};

class CursorInfo
{
public:
    enum JSCursorKind {
        JSInvalid = 10000,
        JSDeclaration,
        JSReference,
        JSInclude
    };

    enum RPCursorKind {
        Invalid = 20000,
        Function,
        Class,
        Constructor,
        Destructor,
        Variable,
        Member,
        Argument // or struct
    };

    CursorInfo()
        : mData(0)
    {}

    CursorInfo(CursorData *data)
        : mData(data)
    {
        assert(data);
        assert(data->refCount == 1);
    }

    CursorInfo(const CursorInfo &other)
        : mData(other.mData)
    {
        if (mData)
            mData->ref();
    }

    ~CursorInfo()
    {
        clear();
    }

    CursorInfo &operator=(const CursorInfo &other)
    {
        clear();
        mData = other.mData;
        if (mData)
            mData->ref();
        return *this;
    }

    CursorData *detach()
    {
        if (!mData) {
            mData = new CursorData;
        } else {
            mData->detach();
        }
        return mData;
    }

    void init(int start, int end,
              int symbolLength, const String &symbolName, uint16_t kind,
              int64_t enumValueDefinition, const Set<Location> &targets,
              const Set<Location> &references, CXTypeKind type = CXType_Invalid)
    {
        detach();
        mData->start = start;
        mData->end = end;
        mData->symbolLength = symbolLength;
        mData->symbolName = symbolName;
        mData->kind = kind;
        mData->enumValue = enumValueDefinition;
        mData->targets = targets;
        mData->references = references;
        mData->type = type;
    }

    void clear()
    {
        if (mData) {
            mData->deref();
            mData = 0;
        }
    }

    String kindSpelling() const { return kindSpelling(kind()); }
    static String kindSpelling(uint16_t kind);
    int start() const { return mData ? mData->start : -1; }
    void setStart(int start)
    {
        detach();
        mData->start = start;
    }

    int end() const { return mData ? mData->end : -1; }
    void setEnd(int end)
    {
        detach();
        mData->end = end;
    }

    void setRange(int start, int end)
    {
        detach();
        mData->start = start;
        mData->end = end;
    }

    uint16_t kind() const { return mData ? mData->kind : 0; }
    void setKind(uint16_t kind)
    {
        detach();
        mData->kind = kind;
    }
    String symbolName() const { return mData ? mData->symbolName : String(); }
    void setSymbolName(const String &symbolName)
    {
        detach();
        mData->symbolName = symbolName;
    }

    int symbolLength() const { return mData ? mData->symbolLength : 0; }
    void setSymbolLength(int symbolLength)
    {
        assert(symbolLength != INT_MAX); // int max means empty cursorInfo
        detach();
        mData->symbolLength = symbolLength;
    }

    bool dirty(const Set<uint32_t> &dirty)
    {
        bool changed = false;
        if (mData) {
            mData->detach();
            Set<Location> *locations[] = { &mData->targets, &mData->references };
            for (int i=0; i<2; ++i) {
                Set<Location> &l = *locations[i];
                Set<Location>::iterator it = l.begin();
                while (it != l.end()) {
                    if (dirty.contains(it->fileId())) {
                        changed = true;
                        l.erase(it++);
                    } else {
                        ++it;
                    }
                }
            }
        }
        return changed;
    }

    String displayName() const;
    Set<Location> targets() const { return mData ? mData->targets : Set<Location>(); }
    Set<Location> references() const { return mData ? mData->references : Set<Location>(); }
    void setTargets(const Set<Location> &targets)
    {
        if (mData) {
            mData->detach();
        } else {
            mData = new CursorData;
        }
        mData->targets = targets;
    }
    bool addTarget(const Location &location)
    {
        if (mData) {
            if (!mData->targets.contains(location)) {
                mData->detach();
                mData->targets.insert(location);
                return true;
            }
            return false;
        }
        mData = new CursorData;
        mData->targets.insert(location);
        return true;
    }
    bool addReference(const Location &location)
    {
        if (mData) {
            if (!mData->references.contains(location)) {
                mData->detach();
                mData->references.insert(location);
                return true;
            }
            return false;
        }
        mData = new CursorData;
        mData->references.insert(location);
        return true;
    }
    void setReferences(const Set<Location> &references)
    {
        if (mData) {
            mData->detach();
        } else {
            mData = new CursorData;
        }
        mData->references = references;
    }

    int targetRank(const CursorInfo &target) const;

    bool isValid() const
    {
        return !isEmpty();
    }

    bool isNull() const
    {
        return isEmpty();
    }

    bool isValid(const Location &location) const;

    CursorInfo bestTarget(const SymbolMap &map, const SymbolMap *errors = 0, Location *loc = 0) const;
    SymbolMap targetInfos(const SymbolMap &map, const SymbolMap *errors = 0) const;
    SymbolMap referenceInfos(const SymbolMap &map, const SymbolMap *errors = 0) const;
    SymbolMap callers(const Location &loc, const SymbolMap &map, const SymbolMap *errors = 0) const;
    SymbolMap allReferences(const Location &loc, const SymbolMap &map, const SymbolMap *errors = 0) const;
    SymbolMap virtuals(const Location &loc, const SymbolMap &map, const SymbolMap *errors = 0) const;
    SymbolMap declarationAndDefinition(const Location &loc, const SymbolMap &map, const SymbolMap *errors = 0) const;

    bool isClass() const
    {
        switch (kind()) {
        case CXCursor_ClassDecl:
        case CXCursor_ClassTemplate:
        case CXCursor_StructDecl:
            return true;
        default:
            break;
        }
        return false;
    }

    inline bool isDefinition() const
    {
        return mData && (mData->kind == CXCursor_EnumConstantDecl || mData->definition);
    }

    bool isEmpty() const
    {
        return !mData || (!mData->symbolLength && mData->targets.isEmpty() && mData->references.isEmpty() && mData->start == -1 && mData->end == -1);
    }

    bool unite(const CursorInfo &other)
    {
        if (!other.mData)
            return false;
        if (!mData) {
            mData = other.mData;
            mData->ref();
            return true;
        }
        mData->detach();
        bool changed = false;
        if (mData->targets.isEmpty() && !other.mData->targets.isEmpty()) {
            mData->targets = other.mData->targets;
            changed = true;
        } else if (!other.mData->targets.isEmpty()) {
            int count = 0;
            mData->targets.unite(other.mData->targets, &count);
            if (count)
                changed = true;
        }

        if (mData->end == -1 && mData->start == -1 && other.mData->start != -1 && other.mData->end != -1) {
            mData->start = other.mData->start;
            mData->end = other.mData->end;
            changed = true;
        }

        if (!mData->symbolLength && other.mData->symbolLength) {
            mData->symbolLength = other.mData->symbolLength;
            mData->kind = other.mData->kind;
            mData->enumValue = other.mData->enumValue;
            mData->type = other.mData->type;
            mData->symbolName = other.mData->symbolName;
            changed = true;
        }
        const int oldSize = mData->references.size();
        if (!oldSize) {
            mData->references = other.mData->references;
            if (!mData->references.isEmpty())
                changed = true;
        } else {
            int inserted = 0;
            mData->references.unite(other.mData->references, &inserted);
            if (inserted)
                changed = true;
        }

        return changed;
    }

    enum Flag {
        IgnoreTargets = 0x1,
        IgnoreReferences = 0x2,
        DefaultFlags = 0x0
    };
    String toString(unsigned cursorInfoFlags = DefaultFlags, unsigned keyFlags = 0) const;
    inline void read(Deserializer &deserializer);
    inline void write(Serializer &serializer) const;
private:
    CursorData *mData;
};

inline void CursorInfo::read(Deserializer &deserializer)
{
    int symbolLength;
    deserializer >> symbolLength;
    if (symbolLength == INT_MAX) {
        clear();
    } else if (mData) {
        mData->detach();
    } else {
        mData = new CursorData;
    }
    mData->symbolLength = symbolLength;
    int type;
    deserializer >> mData->symbolName >> mData->kind >> type
                 >> mData->enumValue >> mData->targets >> mData->references
                 >> mData->start >> mData->end;
    mData->type = static_cast<CXTypeKind>(type);
}

inline void CursorInfo::write(Serializer &serializer) const
{
    if (!mData) {
        serializer << INT_MAX;
    } else {
        serializer << mData->symbolLength << mData->symbolName << mData->kind
                   << static_cast<int>(mData->type) << mData->enumValue << mData->targets
                   << mData->references << mData->start << mData->end;
    }
}

template <> inline Serializer &operator<<(Serializer &s, const CursorInfo &t)
{
    t.write(s);
    return s;
}

template <> inline Deserializer &operator>>(Deserializer &s, CursorInfo &t)
{
    t.read(s);
    return s;
}

inline Log operator<<(Log log, const CursorInfo &info)
{
    log << info.toString();
    return log;
}

#endif
