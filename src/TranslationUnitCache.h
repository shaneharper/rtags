#ifndef TranslationUnitCache_h
#define TranslationUnitCache_h

#include <RTagsClang.h>
#include <rct/Map.h>
#include <condition_variable>
#include <mutex>

class TranslationUnitCache;
class TranslationUnit : public std::enable_shared_from_this<TranslationUnit>
{
public:
    ~TranslationUnit();
    enum State {
        Invalid,
        Parsing,
        Reparsing,
        Completing,
        Ready
    };

    State state() const;
    State transition(State state);
    TranslationUnitCache *cache() const;
    CXIndex index() const;
    CXTranslationUnit translationUnit() const;
private:
    TranslationUnit(CXTranslationUnit unit, TranslationUnitCache *cache);
    friend class TranslationUnitCache;

    std::mutex mMutex;
    State mState;
    TranslationUnitCache *mCache;
    CXTranslationUnit mTranslationUnit;
};
class TranslationUnitCache
{
public:
    TranslationUnitCache(int size);
    ~TranslationUnitCache();

    std::shared_ptr<TranslationUnit> find(uint32_t fileId) const;
    std::shared_ptr<TranslationUnit> find(const SourceInformation &info) const;
private:
    struct CachedUnit {
        List<String> args;
        Path compiler;
        std::shared_ptr<TranslationUnit> unit;
    };
    Map<uint32_t, CachedUnit*> mUnits;
};

#endif
