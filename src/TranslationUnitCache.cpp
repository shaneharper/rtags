#include "TranslationUnitCache.h"
#include "Server.h"

TranslationUnit::TranslationUnit(const SourceInformation &sourceInfo, TranslationUnitCache *cache, CXTranslationUnit unit)
    : mState(Invalid), mCache(cache), mTranslationUnit(unit), mSourceInformation(sourceInfo)
{
}

TranslationUnit::~TranslationUnit()
{
    if (mTranslationUnit)
        clang_disposeTranslationUnit(mTranslationUnit);
}

TranslationUnit::State TranslationUnit::state() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mState;
}

void TranslationUnit::transition(State state, CXTranslationUnit *unit)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mState = state;
    if (unit)
        mTranslationUnit = *unit;
    mCondition.notify_all();
}

TranslationUnitCache * TranslationUnit::cache() const
{
    return mCache;
}

CXIndex TranslationUnit::index() const
{
    return Server::instance()->clangIndex();
}

CXTranslationUnit TranslationUnit::translationUnit() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mTranslationUnit;
}

TranslationUnitCache::TranslationUnitCache(int size)
    : mFirst(0), mLast(0), mMaxSize(size)
{

}

TranslationUnitCache::~TranslationUnitCache()
{
    CachedUnit *unit = mFirst;
    while (unit) {
        CachedUnit *tmp = unit;
        unit = unit->next;
        delete tmp;
    }
}

std::shared_ptr<TranslationUnit> TranslationUnitCache::find(uint32_t fileId)
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (CachedUnit *unit = mUnits.value(fileId))
        return unit->translationUnit;
    return std::shared_ptr<TranslationUnit>();
}

std::shared_ptr<TranslationUnit> TranslationUnitCache::get(const SourceInformation &info)
{
    std::lock_guard<std::mutex> lock(mMutex);
    uint32_t fileId = Location::insertFile(info.sourceFile);
    CachedUnit *unit = mUnits.value(fileId);
    if (unit) {
        const SourceInformation s = unit->translationUnit->sourceInformation();
        if (s.compiler == info.compiler && s.args == info.args)
            return unit->translationUnit;
    }
    return std::shared_ptr<TranslationUnit>();
}

void TranslationUnitCache::insert(const std::shared_ptr<TranslationUnit> &unit)
{

}

void TranslationUnitCache::purge()
{
    std::lock_guard<std::mutex> lock(mMutex);
    while (mUnits.size() > mMaxSize) {
        CachedUnit *tmp = mFirst;
        mFirst = tmp->next;
        delete tmp;
        if (mFirst) {
            mFirst->prev = 0;
        } else {
            mLast = 0;
            assert(mUnits.isEmpty());
        }
    }
}

int TranslationUnitCache::size() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mUnits.size();
}
