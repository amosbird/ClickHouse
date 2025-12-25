#pragma once

#include <Storages/MergeTree/MergeTreeIndexText.h>

namespace DB
{

struct ProjectionDescription;

struct MergeTreeIndexGranuleProjection final : public MergeTreeIndexGranuleText
{
public:
    explicit MergeTreeIndexGranuleProjection(const ProjectionDescription & projection_);
    ~MergeTreeIndexGranuleProjection() override;

    void deserializeBinaryWithMultipleStreams(MergeTreeIndexInputStreams & streams, MergeTreeIndexDeserializationState & state) override;

private:
    const ProjectionDescription & projection;
};

class MergeTreeIndexProjection final : public IMergeTreeIndex
{
public:
    explicit MergeTreeIndexProjection(const ProjectionDescription & projection_);

    ~MergeTreeIndexProjection() override = default;

    bool supportsReadingOnParallelReplicas() const override { return true; }
    bool isTextIndex() const override { return true; }

    MergeTreeIndexSubstreams getSubstreams() const override;
    MergeTreeIndexFormat
    getDeserializedFormat(const MergeTreeDataPartChecksums & checksums, const std::string & path_prefix) const override;

    MergeTreeIndexGranulePtr createIndexGranule() const override;
    MergeTreeIndexAggregatorPtr createIndexAggregator() const override;
    MergeTreeIndexConditionPtr createIndexCondition(const ActionsDAG::Node * predicate, ContextPtr context) const override;

    const ProjectionDescription & projection;
    std::shared_ptr<const MergeTreeIndexText> text_index;
};

}
