/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/lite_parsed_pipeline.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/stats/counters.h"

namespace mongo {

ReadConcernSupportResult LiteParsedPipeline::supportsReadConcern(
    repl::ReadConcernLevel level,
    boost::optional<ExplainOptions::Verbosity> explain,
    bool enableMajorityReadConcern) const {
    // Start by assuming that we will support both readConcern and cluster-wide default.
    ReadConcernSupportResult result = ReadConcernSupportResult::allSupportedAndDefaultPermitted();

    // 1. Determine whether the given read concern must be rejected for any pipeline-global reasons.
    if (!hasChangeStream() && !enableMajorityReadConcern &&
        (level == repl::ReadConcernLevel::kMajorityReadConcern)) {
        // Reject non change stream aggregation queries that try to use "majority" read concern when
        // enableMajorityReadConcern=false.
        result.readConcernSupport = {
            ErrorCodes::ReadConcernMajorityNotEnabled,
            "Only change stream aggregation queries support 'majority' read concern when "
            "enableMajorityReadConcern=false"};
    } else if (explain && level != repl::ReadConcernLevel::kLocalReadConcern) {
        // Reject non-local read concern when the pipeline is being explained.
        result.readConcernSupport = {
            ErrorCodes::InvalidOptions,
            str::stream() << "Explain for the aggregate command cannot run with a readConcern "
                          << "other than 'local'. Current readConcern level: "
                          << repl::readConcernLevels::toString(level)};
    }

    // 2. Determine whether the default read concern must be denied for any pipeline-global reasons.
    if (explain) {
        result.defaultReadConcernPermit = {
            ErrorCodes::InvalidOptions,
            "Explain for the aggregate command does not permit default readConcern to be "
            "applied."};
    }

    // 3. If either the specified or default readConcern have not already been rejected, determine
    // whether the pipeline stages support them. If not, we record the first error we encounter.
    for (auto&& spec : _stageSpecs) {
        // If both result statuses are already not OK, stop checking further stages.
        if (!result.readConcernSupport.isOK() && !result.defaultReadConcernPermit.isOK()) {
            break;
        }
        result.merge(spec->supportsReadConcern(level));
    }

    return result;
}

void LiteParsedPipeline::assertSupportsMultiDocumentTransaction(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            "Operation not permitted in transaction :: caused by :: Explain for the aggregate "
            "command cannot run within a multi-document transaction",
            !explain);

    for (auto&& spec : _stageSpecs) {
        spec->assertSupportsMultiDocumentTransaction();
    }
}

void LiteParsedPipeline::verifyIsSupported(
    OperationContext* opCtx,
    const std::function<bool(OperationContext*, const NamespaceString&)> isSharded,
    const boost::optional<ExplainOptions::Verbosity> explain,
    bool enableMajorityReadConcern) const {
    // Verify litePipe can be run in a transaction.
    if (opCtx->inMultiDocumentTransaction()) {
        assertSupportsMultiDocumentTransaction(explain);
    }
    // Verify that no involved namespace is sharded unless allowed by the pipeline.
    for (const auto& nss : getInvolvedNamespaces()) {
        uassert(28769,
                str::stream() << nss.ns() << " cannot be sharded",
                allowShardedForeignCollection(nss) || !isSharded(opCtx, nss));
    }
}

void LiteParsedPipeline::tickGlobalStageCounters() const {
    for (auto&& stage : _stageSpecs) {
        // Tick counter corresponding to current stage.
        aggStageCounters.stageCounterMap.find(stage->getParseTimeName())
            ->second->counter.increment(1);

        // Recursively step through any sub-pipelines.
        for (auto&& subPipeline : stage->getSubPipelines()) {
            subPipeline.tickGlobalStageCounters();
        }
    }
}

void LiteParsedPipeline::validate(const OperationContext* opCtx,
                                  bool performApiVersionChecks) const {
    // An internal client could be one of the following :
    //     - Does not have any transport session
    //     - The transport session tag is internal
    auto client = opCtx->getClient();
    const auto isInternalClient =
        !client->session() || (client->session()->getTags() & transport::Session::kInternalClient);

    const auto apiParameters = APIParameters::get(opCtx);
    auto apiVersion = apiParameters.getAPIVersion().value_or("");
    auto apiStrict = apiParameters.getAPIStrict().value_or(false);
    using AllowanceFlags = LiteParsedDocumentSource::AllowedWithApiStrict;

    int internalUnpackBucketCount = 0;
    for (auto&& stage : _stageSpecs) {
        const auto& stageName = stage->getParseTimeName();
        const auto& stageInfo = LiteParsedDocumentSource::getInfo(stageName);

        uassert(5491300,
                str::stream() << "The stage '" << stageName << "' is not allowed in user requests",
                !(stageInfo.allowedWithClientType ==
                      LiteParsedDocumentSource::AllowedWithClientType::kInternal &&
                  !isInternalClient));

        // Validate that the stage is API version compatible.
        if (performApiVersionChecks && apiStrict) {
            switch (stageInfo.allowedWithApiStrict) {
                case AllowanceFlags::kNeverInVersion1: {
                    uassert(ErrorCodes::APIStrictError,
                            str::stream()
                                << "stage " << stageName
                                << " is not allowed with 'apiStrict: true' in API Version "
                                << apiVersion,
                            apiVersion != "1");
                    break;
                }
                case AllowanceFlags::kInternal: {
                    uassert(ErrorCodes::APIStrictError,
                            str::stream()
                                << "Internal stage " << stageName
                                << " cannot be specified with 'apiStrict: true' in API Version "
                                << apiVersion,
                            isInternalClient);
                    break;
                }
                case AllowanceFlags::kAlways: {
                    break;
                }
            }
        }

        internalUnpackBucketCount +=
            (DocumentSourceInternalUnpackBucket::kStageName == stageName) ? 1 : 0;

        for (auto&& subPipeline : stage->getSubPipelines()) {
            subPipeline.validate(opCtx, performApiVersionChecks);
        }
    }


    // Validates that the pipeline contains at most one $_internalUnpackBucket stage.
    uassert(5348302,
            str::stream() << "Encountered pipeline with more than one "
                          << DocumentSourceInternalUnpackBucket::kStageName << " stage",
            internalUnpackBucketCount <= 1);
}

}  // namespace mongo
