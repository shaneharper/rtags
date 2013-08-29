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
    void transition(State state, CXTranslationUnit unit) { transition(state, &unit); }
    void transition(State state) { transition(state, static_cast<CXTranslationUnit*>(0)); }
    TranslationUnitCache *cache() const;
    CXIndex index() const;
    CXTranslationUnit translationUnit() const;
    SourceInformation sourceInformation() const { return mSourceInformation; }
    uint32_t fileId() const { return mSourceInformation.fileId; }
private:
    TranslationUnit(const SourceInformation &sourceInfo, TranslationUnitCache *cache, CXTranslationUnit unit = 0);

    void transition(State state, CXTranslationUnit *unit);

    std::condition_variable mCondition;
    mutable std::mutex mMutex;
    State mState;
    TranslationUnitCache *mCache;
    CXTranslationUnit mTranslationUnit;
    const SourceInformation mSourceInformation;

    friend class TranslationUnitCache;
};

class TranslationUnitCache
{
public:
    TranslationUnitCache(int size);
    ~TranslationUnitCache();

    std::shared_ptr<TranslationUnit> find(uint32_t fileId);
    std::shared_ptr<TranslationUnit> get(const SourceInformation &info);
    void insert(const std::shared_ptr<TranslationUnit> &unit);

    int maxSize() const { return mMaxSize; }
    int size() const;
private:
    struct CachedUnit;
    void moveToEnd(CachedUnit *unit);
    void purge();
    struct CachedUnit {
        std::shared_ptr<TranslationUnit> translationUnit;
        CachedUnit *prev, *next;
    };

    mutable std::mutex mMutex;
    CachedUnit *mFirst, *mLast;
    const int mMaxSize;
    Map<uint32_t, CachedUnit*> mUnits;
};

#endif
