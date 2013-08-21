#include "TranslationUnitCache.h"

TranslationUnit::TranslationUnit(CXTranslationUnit unit, TranslationUnitCache *cache)
    : mState(Invalid), mCache(cache), mTranslationUnit(unit)
{
}

TranslationUnit::~TranslationUnit()
{
    if (mTranslationUnit)
        clang_disposeTranslationUnit(mTranslationUnit);
}

TranslationUnit::State TranslationUnit::state() const
{

}

TranslationUnit::State TranslationUnit::transition(State state)
{

}

TranslationUnitCache * TranslationUnit::cache() const
{

}

CXIndex TranslationUnit::index() const
{

}

CXTranslationUnit TranslationUnit::translationUnit() const
{

}

TranslationUnitCache::TranslationUnitCache(int size)
{

}

TranslationUnitCache::~TranslationUnitCache()
{

}
