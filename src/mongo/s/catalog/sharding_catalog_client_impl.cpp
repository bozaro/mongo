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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/sharding_catalog_client_impl.h"

#include <iomanip>
#include <pcrecpp.h>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/commands.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source_facet.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/executor/network_interface.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/s/catalog/config_server_version.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/set_shard_version_request.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/shard_util.h"
#include "mongo/s/write_ops/batch_write_op.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(failApplyChunkOps);

using repl::OpTime;
using std::set;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;
using str::stream;

namespace {

const ReadPreferenceSetting kConfigReadSelector(ReadPreference::Nearest, TagSet{});
const ReadPreferenceSetting kConfigPrimaryPreferredSelector(ReadPreference::PrimaryPreferred,
                                                            TagSet{});
const int kMaxReadRetry = 3;
const int kMaxWriteRetry = 3;

const NamespaceString kSettingsNamespace("config", "settings");

void toBatchError(const Status& status, BatchedCommandResponse* response) {
    response->clear();
    response->setStatus(status);
}

void sendRetryableWriteBatchRequestToConfig(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            std::vector<BSONObj>& docs,
                                            TxnNumber txnNumber,
                                            const WriteConcernOptions& writeConcern) {
    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setDocuments(docs);
        return insertOp;
    }());
    request.setWriteConcern(writeConcern.toBSON());

    BSONObj cmdObj = request.toBSON();
    BSONObjBuilder bob(cmdObj);
    bob.append(OperationSessionInfo::kTxnNumberFieldName, txnNumber);

    BatchedCommandResponse batchResponse;
    auto response = configShard->runCommand(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            nss.db().toString(),
                                            bob.obj(),
                                            Shard::kDefaultConfigCommandTimeout,
                                            Shard::RetryPolicy::kIdempotent);

    auto writeStatus = Shard::CommandResponse::processBatchWriteResponse(response, &batchResponse);

    uassertStatusOK(batchResponse.toStatus());
    uassertStatusOK(writeStatus);
}

AggregateCommand makeCollectionAndChunksAggregation(OperationContext* opCtx,
                                                    const NamespaceString& nss,
                                                    const ChunkVersion& sinceVersion) {
    auto expCtx = make_intrusive<ExpressionContext>(opCtx, nullptr, nss);
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    resolvedNamespaces[CollectionType::ConfigNS.coll()] = {CollectionType::ConfigNS,
                                                           std::vector<BSONObj>()};
    resolvedNamespaces[ChunkType::ConfigNS.coll()] = {ChunkType::ConfigNS, std::vector<BSONObj>()};
    expCtx->setResolvedNamespaces(resolvedNamespaces);

    using Doc = Document;
    using Arr = std::vector<Value>;

    Pipeline::SourceContainer stages;

    // 1. Match config.collections entries with {_id: nss}. At most one will match.
    // {
    //     "$match": {
    //         "_id": nss
    //     }
    // }
    stages.emplace_back(DocumentSourceMatch::create(Doc{{"_id", nss.toString()}}.toBson(), expCtx));

    // 2. Lookup chunks in config.chunks for the matched collection. Match chunks by 'uuid' or 'ns'
    // depending on whether the collection entry has 'timestamp' or not. If the collection entry has
    // the same 'lastmodEpoch' as 'sinceVersion', then match only chunks with 'lastmod' greater or
    // equal to Timestamp(sinceVersion).
    // Because of SERVER-34926, a $lookup that uses an $expr operator together with a range match
    // query won't be able to use indexes. To work around this, we use a $facet to create 4
    // different 'branches' depending on whether we match by 'ns' or 'uuid' and whether the refresh
    // is incremental or not. This way, in each one of the $facet subpipelines we don't need to use
    // the $expr operator in the $gte range comparison. Since the match conditions in each one of
    // the $facet branches are mutually exclusive, only one of them will execute.
    //
    // {
    //     "$facet": {
    //         "collWithUUIDIncremental": [
    //             {
    //                 "$match": {
    //                     "timestamp": {
    //                         "$exists": 1
    //                     },
    //                     "lastmodEpoch": <sinceVersion.epoch>
    //                 }
    //             },
    //             {
    //                 "$lookup": {
    //                     "from": "chunks",
    //                     "as": "chunks",
    //                     "let": {
    //                         "local_uuid": "$uuid"
    //                     },
    //                     "pipeline": [
    //                         {
    //                             "$match": {
    //                                 "$expr": {
    //                                     "$eq": [
    //                                         "$uuid",
    //                                         "$$local_uuid"
    //                                     ]
    //                                 },
    //                                 "lastmod": {
    //                                     "$gte": <Timestamp(sinceVersion)>
    //                                 }
    //                             }
    //                         },
    //                         {
    //                             "$sort": {
    //                                 "lastmod": 1
    //                             }
    //                         }
    //                     ]
    //                 }
    //             }
    //         ],
    //         "collWithUUIDNonIncremental": [
    //             {
    //                 "$match": {
    //                     "timestamp": {
    //                         "$exists": 1
    //                     },
    //                     "lastmodEpoch": {
    //                         "$ne": <sinceVersion.epoch>
    //                     }
    //                 }
    //             },
    //             {
    //                 "$lookup": {
    //                     "from": "chunks",
    //                     "as": "chunks",
    //                     "let": {
    //                         "local_uuid": "$uuid"
    //                     },
    //                     "pipeline": [
    //                         {
    //                             "$match": {
    //                                 "$expr": {
    //                                     "$eq": [
    //                                         "$uuid",
    //                                         "$$local_uuid"
    //                                     ]
    //                                 }
    //                             }
    //                         },
    //                         {
    //                             "$sort": {
    //                                 "lastmod": 1
    //                             }
    //                         }
    //                     ]
    //                 }
    //             }
    //         ],
    //         "collWithNsIncremental": [...],
    //         "collWithNsNonIncremental": [...]
    //     }
    // }
    constexpr auto chunksLookupOutputFieldName = "chunks"_sd;
    const auto buildLookUpStageFn = [&](bool withUUID, bool incremental) {
        const auto letExpr =
            withUUID ? Doc{{"local_uuid", "$uuid"_sd}} : Doc{{"local_ns", "$_id"_sd}};
        const auto eqNsOrUuidExpr = withUUID ? Arr{Value{"$uuid"_sd}, Value{"$$local_uuid"_sd}}
                                             : Arr{Value{"$ns"_sd}, Value{"$$local_ns"_sd}};
        auto pipelineMatchExpr = [&]() {
            if (incremental) {
                return Doc{{"$expr", Doc{{"$eq", eqNsOrUuidExpr}}},
                           {"lastmod", Doc{{"$gte", Timestamp(sinceVersion.toLong())}}}};
            } else {
                return Doc{{"$expr", Doc{{"$eq", eqNsOrUuidExpr}}}};
            }
        }();

        return Doc{{"from", ChunkType::ConfigNS.coll()},
                   {"as", chunksLookupOutputFieldName},
                   {"let", letExpr},
                   {"pipeline",
                    Arr{Value{Doc{{"$match", pipelineMatchExpr}}},
                        Value{Doc{{"$sort", Doc{{"lastmod", 1}}}}}}}};
    };

    constexpr auto collWithNsIncrementalFacetName = "collWithNsIncremental"_sd;
    constexpr auto collWithUUIDIncrementalFacetName = "collWithUUIDIncremental"_sd;
    constexpr auto collWithNsNonIncrementalFacetName = "collWithNsNonIncremental"_sd;
    constexpr auto collWithUUIDNonIncrementalFacetName = "collWithUUIDNonIncremental"_sd;

    stages.emplace_back(DocumentSourceFacet::createFromBson(
        // TODO SERVER-53283: Once 5.0 has branched out, the 'collWithNsIncremental' and
        // 'collWithNsNonIncremental' branches are no longer needed.
        Doc{{"$facet",
             Doc{{collWithNsIncrementalFacetName,
                  Arr{Value{Doc{{"$match",
                                 Doc{{"timestamp", Doc{{"$exists", 0}}},
                                     {"lastmodEpoch", sinceVersion.epoch()}}}}},
                      Value{Doc{
                          {"$lookup",
                           buildLookUpStageFn(false /* withUuid */, true /* incremental */)}}}}},
                 {collWithUUIDIncrementalFacetName,
                  Arr{Value{Doc{{"$match",
                                 Doc{{"timestamp", Doc{{"$exists", 1}}},
                                     {"lastmodEpoch", sinceVersion.epoch()}}}}},
                      Value{
                          Doc{{"$lookup",
                               buildLookUpStageFn(true /* withUuid */, true /* incremental */)}}}}},
                 {collWithNsNonIncrementalFacetName,
                  Arr{Value{Doc{{"$match",
                                 Doc{{"timestamp", Doc{{"$exists", 0}}},
                                     {"lastmodEpoch", Doc{{"$ne", sinceVersion.epoch()}}}}}}},
                      Value{Doc{
                          {"$lookup",
                           buildLookUpStageFn(false /* withUuid */, false /* incremental */)}}}}},
                 {collWithUUIDNonIncrementalFacetName,
                  Arr{Value{Doc{{"$match",
                                 Doc{{"timestamp", Doc{{"$exists", 1}}},
                                     {"lastmodEpoch", Doc{{"$ne", sinceVersion.epoch()}}}}}}},
                      Value{Doc{
                          {"$lookup",
                           buildLookUpStageFn(true /* withUuid */, false /* incremental */)}}}}}}}}
            .toBson()
            .firstElement(),
        expCtx));

    // 3. Collapse the arrays output by $facet (only one of them has an element) into a single array
    // 'coll'.
    // {
    //     "$project": {
    //         "_id": true,
    //         "coll": {
    //             "$setUnion": [
    //                 "$collWithNsIncremental",
    //                 "$collWithUUIDIncremental",
    //                 "$collWithNsNonIncremental",
    //                 "$collWithUUIDNonIncremental"
    //             ]
    //         }
    //     }
    // }
    stages.emplace_back(DocumentSourceProject::createFromBson(
        Doc{{"$project",
             Doc{{"coll",
                  Doc{{"$setUnion",
                       Arr{Value{"$" + collWithNsIncrementalFacetName},
                           Value{"$" + collWithUUIDIncrementalFacetName},
                           Value{"$" + collWithNsNonIncrementalFacetName},
                           Value{"$" + collWithUUIDNonIncrementalFacetName}}}}}}}}
            .toBson()
            .firstElement(),
        expCtx));

    // 4. Unwind the 'coll' array (which has at most one element).
    // {
    //     "$unwind": {
    //         "path": "$coll"
    //     }
    // }
    stages.emplace_back(DocumentSourceUnwind::createFromBson(
        Doc{{"$unwind", Doc{{"path", "$coll"_sd}}}}.toBson().firstElement(), expCtx));

    // 5. Promote the 'coll' document to the top level.
    // {
    //     "$replaceRoot": {
    //         "newRoot": "$coll"
    //     }
    // }
    stages.emplace_back(DocumentSourceReplaceRoot::createFromBson(
        Doc{{"$replaceRoot", Doc{{"newRoot", "$coll"_sd}}}}.toBson().firstElement(), expCtx));

    // 6. Unwind the 'chunks' array.
    // {
    //     "$unwind": {
    //         "path": "$chunks",
    //         "preserveNullAndEmptyArrays": true,
    //         "includeArrayIndex": "chunksArrayIndex"
    //     }
    // }
    constexpr auto chunksArrayIndexFieldName = "chunksArrayIndex"_sd;
    stages.emplace_back(DocumentSourceUnwind::createFromBson(
        Doc{{"$unwind",
             Doc{{"path", "$" + chunksLookupOutputFieldName},
                 {"preserveNullAndEmptyArrays", true},
                 {"includeArrayIndex", chunksArrayIndexFieldName}}}}
            .toBson()
            .firstElement(),
        expCtx));

    // 7. After unwinding the chunks we are left with the same collection metadata repeated for each
    // one of the chunks. To reduce the size of the response, only keep the collection metadata for
    // the first result entry and omit it from the following ones.
    // {
    //     $replaceRoot: {
    //         newRoot: {$cond: [{$gt: ["$chunksArrayIndex", 0]}, {chunks: "$chunks"}, "$$ROOT"]}
    //     }
    // }
    stages.emplace_back(DocumentSourceReplaceRoot::createFromBson(
        Doc{{"$replaceRoot",
             Doc{{"newRoot",
                  Doc{{"$cond",
                       Arr{Value{
                               Doc{{"$gt", Arr{Value{"$" + chunksArrayIndexFieldName}, Value{0}}}}},
                           Value{Doc{
                               {chunksLookupOutputFieldName, "$" + chunksLookupOutputFieldName}}},
                           Value{"$$ROOT"_sd}}}}}}}}
            .toBson()
            .firstElement(),
        expCtx));

    auto pipeline = Pipeline::create(std::move(stages), expCtx);
    auto serializedPipeline = pipeline->serializeToBson();
    return AggregateCommand(CollectionType::ConfigNS, std::move(serializedPipeline));
}

}  // namespace

ShardingCatalogClientImpl::ShardingCatalogClientImpl() = default;

ShardingCatalogClientImpl::~ShardingCatalogClientImpl() = default;

Status ShardingCatalogClientImpl::updateShardingCatalogEntryForCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionType& coll,
    const bool upsert) {
    auto status = _updateConfigDocument(opCtx,
                                        CollectionType::ConfigNS,
                                        BSON(CollectionType::kNssFieldName << nss.ns()),
                                        coll.toBSON(),
                                        upsert,
                                        ShardingCatalogClient::kMajorityWriteConcern);
    return status.getStatus().withContext(str::stream() << "Collection metadata write failed");
}

DatabaseType ShardingCatalogClientImpl::getDatabase(OperationContext* opCtx,
                                                    StringData dbName,
                                                    repl::ReadConcernLevel readConcernLevel) {
    uassert(ErrorCodes::InvalidNamespace,
            stream() << dbName << " is not a valid db name",
            NamespaceString::validDBName(dbName, NamespaceString::DollarInDbNameBehavior::Allow));

    // The admin database is always hosted on the config server.
    if (dbName == NamespaceString::kAdminDb)
        return DatabaseType(
            dbName.toString(), ShardId::kConfigServerId, false, DatabaseVersion::makeFixed());

    // The config database's primary shard is always config, and it is always sharded.
    if (dbName == NamespaceString::kConfigDb)
        return DatabaseType(
            dbName.toString(), ShardId::kConfigServerId, true, DatabaseVersion::makeFixed());

    auto result =
        _fetchDatabaseMetadata(opCtx, dbName.toString(), kConfigReadSelector, readConcernLevel);
    if (result == ErrorCodes::NamespaceNotFound) {
        // If we failed to find the database metadata on the 'nearest' config server, try again
        // against the primary, in case the database was recently created.
        return uassertStatusOK(
                   _fetchDatabaseMetadata(opCtx,
                                          dbName.toString(),
                                          ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                          readConcernLevel))
            .value;
    }

    return uassertStatusOK(std::move(result)).value;
}

std::vector<DatabaseType> ShardingCatalogClientImpl::getAllDBs(OperationContext* opCtx,
                                                               repl::ReadConcernLevel readConcern) {
    std::vector<DatabaseType> databases;
    auto dbs = uassertStatusOK(_exhaustiveFindOnConfig(opCtx,
                                                       kConfigReadSelector,
                                                       readConcern,
                                                       DatabaseType::ConfigNS,
                                                       BSONObj(),
                                                       BSONObj(),
                                                       boost::none))
                   .value;
    for (const BSONObj& doc : dbs) {
        auto db = uassertStatusOKWithContext(
            DatabaseType::fromBSON(doc), stream() << "Failed to parse database document " << doc);
        uassertStatusOKWithContext(db.validate(),
                                   stream() << "Failed to validate database document " << doc);

        databases.emplace_back(std::move(db));
    }

    return databases;
}

StatusWith<repl::OpTimeWith<DatabaseType>> ShardingCatalogClientImpl::_fetchDatabaseMetadata(
    OperationContext* opCtx,
    const std::string& dbName,
    const ReadPreferenceSetting& readPref,
    repl::ReadConcernLevel readConcernLevel) {
    invariant(dbName != NamespaceString::kAdminDb && dbName != NamespaceString::kConfigDb);

    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              readPref,
                                              readConcernLevel,
                                              DatabaseType::ConfigNS,
                                              BSON(DatabaseType::name(dbName)),
                                              BSONObj(),
                                              boost::none);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docsWithOpTime = findStatus.getValue();
    if (docsWithOpTime.value.empty()) {
        return {ErrorCodes::NamespaceNotFound, stream() << "database " << dbName << " not found"};
    }

    invariant(docsWithOpTime.value.size() == 1);

    auto parseStatus = DatabaseType::fromBSON(docsWithOpTime.value.front());
    if (!parseStatus.isOK()) {
        return parseStatus.getStatus();
    }

    return repl::OpTimeWith<DatabaseType>(parseStatus.getValue(), docsWithOpTime.opTime);
}

CollectionType ShardingCatalogClientImpl::getCollection(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        repl::ReadConcernLevel readConcernLevel) {
    auto collDoc =
        uassertStatusOK(_exhaustiveFindOnConfig(opCtx,
                                                kConfigReadSelector,
                                                readConcernLevel,
                                                CollectionType::ConfigNS,
                                                BSON(CollectionType::kNssFieldName << nss.ns()),
                                                BSONObj(),
                                                1))
            .value;
    uassert(ErrorCodes::NamespaceNotFound,
            stream() << "collection " << nss.ns() << " not found",
            !collDoc.empty());

    CollectionType coll(collDoc[0]);
    uassert(ErrorCodes::NamespaceNotFound,
            stream() << "collection " << nss.ns() << " was dropped",
            !coll.getDropped());
    return coll;
}

std::vector<CollectionType> ShardingCatalogClientImpl::getCollections(
    OperationContext* opCtx, StringData dbName, repl::ReadConcernLevel readConcernLevel) {
    BSONObjBuilder b;
    if (!dbName.empty())
        b.appendRegex(CollectionType::kNssFieldName,
                      std::string(str::stream()
                                  << "^" << pcrecpp::RE::QuoteMeta(dbName.toString()) << "\\."));

    auto collDocs = uassertStatusOK(_exhaustiveFindOnConfig(opCtx,
                                                            kConfigReadSelector,
                                                            readConcernLevel,
                                                            CollectionType::ConfigNS,
                                                            b.obj(),
                                                            BSONObj(),
                                                            boost::none))
                        .value;
    std::vector<CollectionType> collections;
    for (const BSONObj& obj : collDocs)
        collections.emplace_back(obj);

    return collections;
}

std::vector<NamespaceString> ShardingCatalogClientImpl::getAllShardedCollectionsForDb(
    OperationContext* opCtx, StringData dbName, repl::ReadConcernLevel readConcern) {
    auto collectionsOnConfig = getCollections(opCtx, dbName, readConcern);

    std::vector<NamespaceString> collectionsToReturn;
    for (const auto& coll : collectionsOnConfig) {
        if (coll.getDropped()) {
            continue;
        }

        collectionsToReturn.push_back(coll.getNss());
    }

    return collectionsToReturn;
}

StatusWith<BSONObj> ShardingCatalogClientImpl::getGlobalSettings(OperationContext* opCtx,
                                                                 StringData key) {
    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              kConfigReadSelector,
                                              repl::ReadConcernLevel::kMajorityReadConcern,
                                              kSettingsNamespace,
                                              BSON("_id" << key),
                                              BSONObj(),
                                              1);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docs = findStatus.getValue().value;
    if (docs.empty()) {
        return {ErrorCodes::NoMatchingDocument,
                str::stream() << "can't find settings document with key: " << key};
    }

    invariant(docs.size() == 1);
    return docs.front();
}

StatusWith<VersionType> ShardingCatalogClientImpl::getConfigVersion(
    OperationContext* opCtx, repl::ReadConcernLevel readConcern) {
    auto findStatus = Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
        opCtx,
        kConfigReadSelector,
        readConcern,
        VersionType::ConfigNS,
        BSONObj(),
        BSONObj(),
        boost::none /* no limit */);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    auto queryResults = findStatus.getValue().docs;

    if (queryResults.size() > 1) {
        return {ErrorCodes::TooManyMatchingDocuments,
                str::stream() << "should only have 1 document in " << VersionType::ConfigNS.ns()};
    }

    if (queryResults.empty()) {
        VersionType versionInfo;
        versionInfo.setMinCompatibleVersion(UpgradeHistory_EmptyVersion);
        versionInfo.setCurrentVersion(UpgradeHistory_EmptyVersion);
        versionInfo.setClusterId(OID{});
        return versionInfo;
    }

    BSONObj versionDoc = queryResults.front();
    auto versionTypeResult = VersionType::fromBSON(versionDoc);
    if (!versionTypeResult.isOK()) {
        return versionTypeResult.getStatus().withContext(
            str::stream() << "Unable to parse config.version document " << versionDoc);
    }

    auto validationStatus = versionTypeResult.getValue().validate();
    if (!validationStatus.isOK()) {
        return Status(validationStatus.withContext(
            str::stream() << "Unable to validate config.version document " << versionDoc));
    }

    return versionTypeResult.getValue();
}

StatusWith<std::vector<std::string>> ShardingCatalogClientImpl::getDatabasesForShard(
    OperationContext* opCtx, const ShardId& shardId) {
    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              kConfigReadSelector,
                                              repl::ReadConcernLevel::kMajorityReadConcern,
                                              DatabaseType::ConfigNS,
                                              BSON(DatabaseType::primary(shardId.toString())),
                                              BSONObj(),
                                              boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    std::vector<std::string> dbs;
    for (const BSONObj& obj : findStatus.getValue().value) {
        string dbName;
        Status status = bsonExtractStringField(obj, DatabaseType::name(), &dbName);
        if (!status.isOK()) {
            return status;
        }

        dbs.push_back(dbName);
    }

    return dbs;
}

StatusWith<std::vector<ChunkType>> ShardingCatalogClientImpl::getChunks(
    OperationContext* opCtx,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<int> limit,
    OpTime* opTime,
    repl::ReadConcernLevel readConcern,
    const boost::optional<BSONObj>& hint) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ConfigServer ||
              readConcern == repl::ReadConcernLevel::kMajorityReadConcern);

    // Convert boost::optional<int> to boost::optional<long long>.
    auto longLimit = limit ? boost::optional<long long>(*limit) : boost::none;
    auto findStatus = _exhaustiveFindOnConfig(
        opCtx, kConfigReadSelector, readConcern, ChunkType::ConfigNS, query, sort, longLimit, hint);
    if (!findStatus.isOK()) {
        return findStatus.getStatus().withContext("Failed to load chunks");
    }

    const auto& chunkDocsOpTimePair = findStatus.getValue();

    std::vector<ChunkType> chunks;
    for (const BSONObj& obj : chunkDocsOpTimePair.value) {
        auto chunkRes = ChunkType::fromConfigBSON(obj);
        if (!chunkRes.isOK()) {
            return chunkRes.getStatus().withContext(stream() << "Failed to parse chunk with id "
                                                             << obj[ChunkType::name()]);
        }

        chunks.push_back(chunkRes.getValue());
    }

    if (opTime) {
        *opTime = chunkDocsOpTimePair.opTime;
    }

    return chunks;
}

std::pair<CollectionType, std::vector<ChunkType>> ShardingCatalogClientImpl::getCollectionAndChunks(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkVersion& sinceVersion,
    const repl::ReadConcernArgs& readConcern) {
    auto aggRequest = makeCollectionAndChunksAggregation(opCtx, nss, sinceVersion);
    aggRequest.setReadConcern(readConcern.toBSONInner());
    aggRequest.setWriteConcern(WriteConcernOptions());

    const auto readPref = (serverGlobalParams.clusterRole == ClusterRole::ConfigServer)
        ? ReadPreferenceSetting()
        : Grid::get(opCtx)->readPreferenceWithConfigTime(kConfigReadSelector);
    aggRequest.setUnwrappedReadPref(readPref.toContainingBSON());

    // Run the aggregation
    std::vector<BSONObj> aggResult;
    auto callback = [&aggResult](const std::vector<BSONObj>& batch) {
        aggResult.insert(aggResult.end(),
                         std::make_move_iterator(batch.begin()),
                         std::make_move_iterator(batch.end()));
        return true;
    };

    const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    for (int retry = 1; retry <= kMaxWriteRetry; retry++) {
        const Status status = configShard->runAggregation(opCtx, aggRequest, callback);
        if (retry < kMaxWriteRetry &&
            configShard->isRetriableError(status.code(), Shard::RetryPolicy::kIdempotent)) {
            aggResult.clear();
            continue;
        }
        uassertStatusOK(status);
        break;
    }

    uassert(ErrorCodes::NamespaceNotFound,
            stream() << "Collection " << nss.ns() << " not found",
            !aggResult.empty());

    // The first aggregation result document has the config.collections entry plus the first
    // returned chunk. Since the CollectionType idl is 'strict: false', it will ignore the foreign
    // 'chunks' field joined onto it.
    const CollectionType coll(aggResult.front());

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Collection " << nss.ns() << " is dropped.",
            !coll.getDropped());

    uassert(ErrorCodes::ConflictingOperationInProgress,
            stream() << "No chunks were found for the collection " << nss,
            aggResult.front().hasField("chunks"));

    std::vector<ChunkType> chunks;
    chunks.reserve(aggResult.size());
    for (const auto& elem : aggResult) {
        const auto chunkElem = elem.getField("chunks");
        if (!chunkElem) {
            // Only the first (and in that case, only) aggregation result may not have chunks. That
            // case is already caught by the uassert above.
            static constexpr auto msg = "No chunks found in aggregation result";
            LOGV2_ERROR(5487400, msg, "elem"_attr = elem);
            uasserted(5487401, msg);
        }

        auto chunkRes = uassertStatusOK(ChunkType::fromConfigBSON(chunkElem.Obj()));
        chunks.emplace_back(std::move(chunkRes));
    }
    return {std::move(coll), std::move(chunks)};
};

StatusWith<std::vector<TagsType>> ShardingCatalogClientImpl::getTagsForCollection(
    OperationContext* opCtx, const NamespaceString& nss) {
    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              kConfigReadSelector,
                                              repl::ReadConcernLevel::kMajorityReadConcern,
                                              TagsType::ConfigNS,
                                              BSON(TagsType::ns(nss.ns())),
                                              BSON(TagsType::min() << 1),
                                              boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus().withContext("Failed to load tags");
    }

    const auto& tagDocsOpTimePair = findStatus.getValue();

    std::vector<TagsType> tags;

    for (const BSONObj& obj : tagDocsOpTimePair.value) {
        auto tagRes = TagsType::fromBSON(obj);
        if (!tagRes.isOK()) {
            return tagRes.getStatus().withContext(str::stream() << "Failed to parse tag with id "
                                                                << obj[TagsType::tag()]);
        }

        tags.push_back(tagRes.getValue());
    }

    return tags;
}

StatusWith<repl::OpTimeWith<std::vector<ShardType>>> ShardingCatalogClientImpl::getAllShards(
    OperationContext* opCtx, repl::ReadConcernLevel readConcern) {
    std::vector<ShardType> shards;
    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              kConfigReadSelector,
                                              readConcern,
                                              ShardType::ConfigNS,
                                              BSONObj(),     // no query filter
                                              BSONObj(),     // no sort
                                              boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    for (const BSONObj& doc : findStatus.getValue().value) {
        auto shardRes = ShardType::fromBSON(doc);
        if (!shardRes.isOK()) {
            return shardRes.getStatus().withContext(stream()
                                                    << "Failed to parse shard document " << doc);
        }

        Status validateStatus = shardRes.getValue().validate();
        if (!validateStatus.isOK()) {
            return validateStatus.withContext(stream()
                                              << "Failed to validate shard document " << doc);
        }

        shards.push_back(shardRes.getValue());
    }

    return repl::OpTimeWith<std::vector<ShardType>>{std::move(shards),
                                                    findStatus.getValue().opTime};
}

Status ShardingCatalogClientImpl::runUserManagementWriteCommand(OperationContext* opCtx,
                                                                StringData commandName,
                                                                StringData dbname,
                                                                const BSONObj& cmdObj,
                                                                BSONObjBuilder* result) {
    BSONObj cmdToRun = cmdObj;
    {
        // Make sure that if the command has a write concern that it is w:1 or w:majority, and
        // convert w:1 or no write concern to w:majority before sending.
        WriteConcernOptions writeConcern;

        BSONElement writeConcernElement = cmdObj[WriteConcernOptions::kWriteConcernField];
        bool initialCmdHadWriteConcern = !writeConcernElement.eoo();
        if (initialCmdHadWriteConcern) {
            auto sw = WriteConcernOptions::parse(writeConcernElement.Obj());
            if (!sw.isOK()) {
                return sw.getStatus();
            }
            writeConcern = sw.getValue();

            if ((writeConcern.wNumNodes != 1) &&
                (writeConcern.wMode != WriteConcernOptions::kMajority)) {
                return {ErrorCodes::InvalidOptions,
                        str::stream() << "Invalid replication write concern. User management write "
                                         "commands may only use w:1 or w:'majority', got: "
                                      << writeConcern.toBSON()};
            }
        }

        writeConcern.wMode = WriteConcernOptions::kMajority;
        writeConcern.wNumNodes = 0;

        BSONObjBuilder modifiedCmd;
        if (!initialCmdHadWriteConcern) {
            modifiedCmd.appendElements(cmdObj);
        } else {
            BSONObjIterator cmdObjIter(cmdObj);
            while (cmdObjIter.more()) {
                BSONElement e = cmdObjIter.next();
                if (WriteConcernOptions::kWriteConcernField == e.fieldName()) {
                    continue;
                }
                modifiedCmd.append(e);
            }
        }
        modifiedCmd.append(WriteConcernOptions::kWriteConcernField, writeConcern.toBSON());
        cmdToRun = modifiedCmd.obj();
    }

    auto swResponse =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            dbname.toString(),
            cmdToRun,
            Shard::kDefaultConfigCommandTimeout,
            Shard::RetryPolicy::kNotIdempotent);

    if (!swResponse.isOK()) {
        return swResponse.getStatus();
    }

    auto response = std::move(swResponse.getValue());

    if (!response.commandStatus.isOK()) {
        return response.commandStatus;
    }

    if (!response.writeConcernStatus.isOK()) {
        return response.writeConcernStatus;
    }

    CommandHelpers::filterCommandReplyForPassthrough(response.response, result);
    return Status::OK();
}

bool ShardingCatalogClientImpl::runUserManagementReadCommand(OperationContext* opCtx,
                                                             const std::string& dbname,
                                                             const BSONObj& cmdObj,
                                                             BSONObjBuilder* result) {
    auto resultStatus =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            kConfigPrimaryPreferredSelector,
            dbname,
            cmdObj,
            Shard::kDefaultConfigCommandTimeout,
            Shard::RetryPolicy::kIdempotent);
    if (resultStatus.isOK()) {
        CommandHelpers::filterCommandReplyForPassthrough(resultStatus.getValue().response, result);
        return resultStatus.getValue().commandStatus.isOK();
    }

    return CommandHelpers::appendCommandStatusNoThrow(*result, resultStatus.getStatus());  // XXX
}

Status ShardingCatalogClientImpl::applyChunkOpsDeprecated(OperationContext* opCtx,
                                                          const BSONArray& updateOps,
                                                          const BSONArray& preCondition,
                                                          const NamespaceString& nss,
                                                          const ChunkVersion& lastChunkVersion,
                                                          const WriteConcernOptions& writeConcern,
                                                          repl::ReadConcernLevel readConcern) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ConfigServer ||
              (readConcern == repl::ReadConcernLevel::kMajorityReadConcern &&
               writeConcern.wMode == WriteConcernOptions::kMajority));
    BSONObj cmd =
        BSON("applyOps" << updateOps << "preCondition" << preCondition
                        << WriteConcernOptions::kWriteConcernField << writeConcern.toBSON());

    auto response =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "config",
            cmd,
            Shard::RetryPolicy::kIdempotent);

    if (!response.isOK()) {
        return response.getStatus();
    }

    Status status = response.getValue().commandStatus.isOK()
        ? std::move(response.getValue().writeConcernStatus)
        : std::move(response.getValue().commandStatus);

    // TODO (Dianna) This fail point needs to be reexamined when CommitChunkMigration is in:
    // migrations will no longer be able to exercise it, so split or merge will need to do so.
    // SERVER-22659.
    if (MONGO_unlikely(failApplyChunkOps.shouldFail())) {
        status = Status(ErrorCodes::InternalError, "Failpoint 'failApplyChunkOps' generated error");
    }

    if (!status.isOK()) {
        string errMsg;

        // This could be a blip in the network connectivity. Check if the commit request made it.
        //
        // If all the updates were successfully written to the chunks collection, the last
        // document in the list of updates should be returned from a query to the chunks
        // collection. The last chunk can be identified by namespace and version number.

        LOGV2_WARNING(
            22675,
            "Error committing chunk operation, metadata will be revalidated. Caused by {error}",
            "Error committing chunk operation, metadata will be revalidated",
            "error"_attr = redact(status));

        // Look for the chunk in this shard whose version got bumped. We assume that if that
        // mod made it to the config server, then transaction was successful.
        BSONObjBuilder query;
        lastChunkVersion.appendLegacyWithField(&query, ChunkType::lastmod());
        query.append(ChunkType::ns(), nss.ns());
        auto chunkWithStatus = getChunks(opCtx, query.obj(), BSONObj(), 1, nullptr, readConcern);

        if (!chunkWithStatus.isOK()) {
            errMsg = str::stream()
                << "getChunks function failed, unable to validate chunk "
                << "operation metadata: " << chunkWithStatus.getStatus().toString()
                << ". applyChunkOpsDeprecated failed to get confirmation "
                << "of commit. Unable to save chunk ops. Command: " << cmd
                << ". Result: " << response.getValue().response;
            return status.withContext(errMsg);
        };

        const auto& newestChunk = chunkWithStatus.getValue();

        if (newestChunk.empty()) {
            errMsg = str::stream()
                << "chunk operation commit failed: version " << lastChunkVersion.toString()
                << " doesn't exist in namespace: " << nss.ns()
                << ". Unable to save chunk ops. Command: " << cmd
                << ". Result: " << response.getValue().response;
            return status.withContext(errMsg);
        };

        invariant(newestChunk.size() == 1);
        return Status::OK();
    }

    return Status::OK();
}

Status ShardingCatalogClientImpl::insertConfigDocument(OperationContext* opCtx,
                                                       const NamespaceString& nss,
                                                       const BSONObj& doc,
                                                       const WriteConcernOptions& writeConcern) {
    invariant(nss.db() == NamespaceString::kAdminDb || nss.db() == NamespaceString::kConfigDb);

    const BSONElement idField = doc.getField("_id");

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setDocuments({doc});
        return insertOp;
    }());
    request.setWriteConcern(writeConcern.toBSON());

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    for (int retry = 1; retry <= kMaxWriteRetry; retry++) {
        auto response = configShard->runBatchWriteCommand(
            opCtx, Shard::kDefaultConfigCommandTimeout, request, Shard::RetryPolicy::kNoRetry);

        Status status = response.toStatus();

        if (retry < kMaxWriteRetry &&
            configShard->isRetriableError(status.code(), Shard::RetryPolicy::kIdempotent)) {
            // Pretend like the operation is idempotent because we're handling DuplicateKey errors
            // specially
            continue;
        }

        // If we get DuplicateKey error on the first attempt to insert, this definitively means that
        // we are trying to insert the same entry a second time, so error out. If it happens on a
        // retry attempt though, it is not clear whether we are actually inserting a duplicate key
        // or it is because we failed to wait for write concern on the first attempt. In order to
        // differentiate, fetch the entry and check.
        if (retry > 1 && status == ErrorCodes::DuplicateKey) {
            LOGV2_DEBUG(
                22674, 1, "Insert retry failed because of duplicate key error, rechecking.");

            auto fetchDuplicate =
                _exhaustiveFindOnConfig(opCtx,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        repl::ReadConcernLevel::kMajorityReadConcern,
                                        nss,
                                        idField.eoo() ? doc : idField.wrap(),
                                        BSONObj(),
                                        boost::none);
            if (!fetchDuplicate.isOK()) {
                return fetchDuplicate.getStatus();
            }

            auto existingDocs = fetchDuplicate.getValue().value;
            if (existingDocs.empty()) {
                return {status.withContext(
                    stream() << "DuplicateKey error was returned after a retry attempt, but no "
                                "documents were found. This means a concurrent change occurred "
                                "together with the retries.")};
            }

            invariant(existingDocs.size() == 1);

            BSONObj existing = std::move(existingDocs.front());
            if (existing.woCompare(doc) == 0) {
                // Documents match, so treat the operation as success
                return Status::OK();
            }
        }

        return status;
    }

    MONGO_UNREACHABLE;
}

void ShardingCatalogClientImpl::insertConfigDocumentsAsRetryableWrite(
    OperationContext* opCtx,
    const NamespaceString& nss,
    std::vector<BSONObj> docs,
    const WriteConcernOptions& writeConcern) {
    invariant(nss.db() == NamespaceString::kAdminDb || nss.db() == NamespaceString::kConfigDb);

    AlternativeSessionRegion asr(opCtx);
    TxnNumber currentTxnNumber = 0;

    std::vector<BSONObj> workingBatch;
    size_t workingBatchItemSize = 0;
    int workingBatchDocSize = 0;

    while (!docs.empty()) {
        BSONObj toAdd = docs.back();
        docs.pop_back();

        const int docSizePlusOverhead =
            toAdd.objsize() + write_ops::kRetryableAndTxnBatchWriteBSONSizeOverhead;
        // Check if pushing this object will exceed the batch size limit or the max object size
        if ((workingBatchItemSize + 1 > write_ops::kMaxWriteBatchSize) ||
            (workingBatchDocSize + docSizePlusOverhead > BSONObjMaxUserSize)) {
            sendRetryableWriteBatchRequestToConfig(
                asr.opCtx(), nss, workingBatch, currentTxnNumber, writeConcern);
            ++currentTxnNumber;

            workingBatch.clear();
            workingBatchItemSize = 0;
            workingBatchDocSize = 0;
        }

        workingBatch.push_back(toAdd);
        ++workingBatchItemSize;
        workingBatchDocSize += docSizePlusOverhead;
    }

    if (!workingBatch.empty()) {
        sendRetryableWriteBatchRequestToConfig(
            asr.opCtx(), nss, workingBatch, currentTxnNumber, writeConcern);
    }
}

StatusWith<bool> ShardingCatalogClientImpl::updateConfigDocument(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& update,
    bool upsert,
    const WriteConcernOptions& writeConcern) {
    return _updateConfigDocument(opCtx, nss, query, update, upsert, writeConcern);
}

StatusWith<bool> ShardingCatalogClientImpl::_updateConfigDocument(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& update,
    bool upsert,
    const WriteConcernOptions& writeConcern) {
    invariant(nss.db() == NamespaceString::kConfigDb);

    BatchedCommandRequest request([&] {
        write_ops::Update updateOp(nss);
        updateOp.setUpdates({[&] {
            write_ops::UpdateOpEntry entry;
            entry.setQ(query);
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(update));
            entry.setUpsert(upsert);
            entry.setMulti(false);
            return entry;
        }()});
        return updateOp;
    }());
    request.setWriteConcern(writeConcern.toBSON());

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto response = configShard->runBatchWriteCommand(
        opCtx, Shard::kDefaultConfigCommandTimeout, request, Shard::RetryPolicy::kIdempotent);

    Status status = response.toStatus();
    if (!status.isOK()) {
        return status;
    }

    const auto nSelected = response.getN();
    invariant(nSelected == 0 || nSelected == 1);
    return (nSelected == 1);
}

Status ShardingCatalogClientImpl::removeConfigDocuments(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        const BSONObj& query,
                                                        const WriteConcernOptions& writeConcern) {
    invariant(nss.db() == NamespaceString::kConfigDb);

    BatchedCommandRequest request([&] {
        write_ops::Delete deleteOp(nss);
        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ(query);
            entry.setMulti(true);
            return entry;
        }()});
        return deleteOp;
    }());
    request.setWriteConcern(writeConcern.toBSON());

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto response = configShard->runBatchWriteCommand(
        opCtx, Shard::kDefaultConfigCommandTimeout, request, Shard::RetryPolicy::kIdempotent);
    return response.toStatus();
}

StatusWith<repl::OpTimeWith<vector<BSONObj>>> ShardingCatalogClientImpl::_exhaustiveFindOnConfig(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const repl::ReadConcernLevel& readConcern,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit,
    const boost::optional<BSONObj>& hint) {
    auto response = Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
        opCtx, readPref, readConcern, nss, query, sort, limit, hint);
    if (!response.isOK()) {
        return response.getStatus();
    }

    return repl::OpTimeWith<vector<BSONObj>>(std::move(response.getValue().docs),
                                             response.getValue().opTime);
}

StatusWith<std::vector<KeysCollectionDocument>> ShardingCatalogClientImpl::getNewKeys(
    OperationContext* opCtx,
    StringData purpose,
    const LogicalTime& newerThanThis,
    repl::ReadConcernLevel readConcernLevel) {
    auto config = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    BSONObjBuilder queryBuilder;
    queryBuilder.append("purpose", purpose);
    queryBuilder.append("expiresAt", BSON("$gt" << newerThanThis.asTimestamp()));

    auto findStatus = config->exhaustiveFindOnConfig(opCtx,
                                                     kConfigReadSelector,
                                                     readConcernLevel,
                                                     NamespaceString::kKeysCollectionNamespace,
                                                     queryBuilder.obj(),
                                                     BSON("expiresAt" << 1),
                                                     boost::none);

    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& keyDocs = findStatus.getValue().docs;
    std::vector<KeysCollectionDocument> keys;
    for (auto&& keyDoc : keyDocs) {
        KeysCollectionDocument key;
        try {
            key = KeysCollectionDocument::parse(IDLParserErrorContext("keyDoc"), keyDoc);
        } catch (...) {
            return exceptionToStatus();
        }
        keys.push_back(std::move(key));
    }

    return keys;
}

}  // namespace mongo
