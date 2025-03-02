/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/global_index.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_applier_utils.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/stats/counters.h"
#include "mongo/util/fail_point.h"

#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


MONGO_FAIL_POINT_DEFINE(hangAfterApplyingCollectionDropOplogEntry);

namespace mongo {
namespace repl {
CachedCollectionProperties::CollectionProperties
CachedCollectionProperties::getCollectionProperties(OperationContext* opCtx,
                                                    const StringMapHashedKey& ns) {
    auto it = _cache.find(ns);
    if (it != _cache.end()) {
        return it->second;
    }

    CollectionProperties collProperties;
    if (auto collection = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(
            opCtx, NamespaceString(ns.key()))) {
        collProperties.isCapped = collection->isCapped();
        collProperties.isClustered = collection->isClustered();
        collProperties.collator = collection->getDefaultCollator();
    }
    _cache[ns] = collProperties;
    return collProperties;
}

void OplogApplierUtils::processCrudOp(
    OperationContext* opCtx,
    OplogEntry* op,
    uint32_t* hash,
    const CachedCollectionProperties::CollectionProperties& collProperties) {
    // Include the _id of the document in the hash so we get parallelism even if all writes are to a
    // single collection.
    //
    // For capped collections, this is usually illegal, since capped collections must preserve
    // insertion order. One exception are clustered capped collections with a monotonically
    // increasing cluster key, which guarantee preservation of the insertion order.
    if (!collProperties.isCapped || collProperties.isClustered) {
        BSONElement id = [&]() {
            if (op->isGlobalIndexCrudOpType()) {
                // The document key indentifies the base collection's document, and is used to
                // serialise index key writes referring to the same document.
                return op->getObject().getField(global_index::kOplogEntryDocKeyFieldName);
            }
            return op->getIdElement();
        }();
        BSONElementComparator elementHasher(BSONElementComparator::FieldNamesMode::kIgnore,
                                            collProperties.collator);
        const size_t idHash = elementHasher.hash(id);
        MurmurHash3_x86_32(&idHash, sizeof(idHash), *hash, hash);
    }

    if (op->getOpType() == OpTypeEnum::kInsert && collProperties.isCapped) {
        // Mark capped collection ops before storing them to ensure we do not attempt to
        // bulk insert them.
        op->setIsForCappedCollection(true);
    }
}

uint32_t getWriterId(OperationContext* opCtx,
                     OplogEntry* op,
                     CachedCollectionProperties* collPropertiesCache,
                     uint32_t numWriters,
                     boost::optional<uint32_t> forceWriterId = boost::none) {
    NamespaceString nss = op->isGlobalIndexCrudOpType()
        ? NamespaceString::makeGlobalIndexNSS(op->getUuid().value())
        : op->getNss();

    // Reduce the hash from 64bit down to 32bit, just to allow combinations with murmur3 later
    // on. Bit depth not important, we end up just doing integer modulo with this in the end.
    // The hash function should provide entropy in the lower bits as it's used in hash tables.
    auto hashedNs = StringMapHasher().hashed_key(nss.ns());
    auto hash = static_cast<uint32_t>(hashedNs.hash());

    if (op->isCrudOpType()) {
        auto collProperties = collPropertiesCache->getCollectionProperties(opCtx, hashedNs);
        OplogApplierUtils::processCrudOp(opCtx, op, &hash, collProperties);
    }

    return (forceWriterId ? *forceWriterId : hash) % numWriters;
}

template <typename Operation>
uint32_t addToWriterVectorImpl(OperationContext* opCtx,
                               OplogEntry* op,
                               std::vector<std::vector<Operation>>* writerVectors,
                               CachedCollectionProperties* collPropertiesCache,
                               boost::optional<uint32_t> forceWriterId = boost::none) {
    auto writerId =
        getWriterId(opCtx, op, collPropertiesCache, writerVectors->size(), forceWriterId);
    auto& writer = (*writerVectors)[writerId];

    if (writer.empty()) {
        // Skip a few growth rounds.
        writer.reserve(8);
    }
    writer.emplace_back(op);

    return writerId;
}

uint32_t OplogApplierUtils::addToWriterVector(
    OperationContext* opCtx,
    OplogEntry* op,
    std::vector<std::vector<const OplogEntry*>>* writerVectors,
    CachedCollectionProperties* collPropertiesCache,
    boost::optional<uint32_t> forceWriterId) {
    return addToWriterVectorImpl(opCtx, op, writerVectors, collPropertiesCache, forceWriterId);
}

uint32_t OplogApplierUtils::addToWriterVector(
    OperationContext* opCtx,
    OplogEntry* op,
    std::vector<std::vector<ApplierOperation>>* writerVectors,
    CachedCollectionProperties* collPropertiesCache,
    boost::optional<uint32_t> forceWriterId) {
    return addToWriterVectorImpl(opCtx, op, writerVectors, collPropertiesCache, forceWriterId);
}

void OplogApplierUtils::stableSortByNamespace(std::vector<ApplierOperation>* oplogEntryPointers) {
    auto nssComparator = [](ApplierOperation l, ApplierOperation r) {
        if (l->getNss().isCommand()) {
            if (r->getNss().isCommand())
                // l == r; now compare the namespace
                return l->getNss() < r->getNss();
            // l < r
            return true;
        }
        if (r->getNss().isCommand())
            // l > r
            return false;
        return l->getNss() < r->getNss();
    };
    std::stable_sort(oplogEntryPointers->begin(), oplogEntryPointers->end(), nssComparator);
}

void OplogApplierUtils::addDerivedOps(OperationContext* opCtx,
                                      std::vector<OplogEntry>* derivedOps,
                                      std::vector<std::vector<ApplierOperation>>* writerVectors,
                                      CachedCollectionProperties* collPropertiesCache,
                                      bool serial) {
    // Used to determine which writer vector to assign serial ops.
    boost::optional<uint32_t> serialWriterId;

    for (auto&& op : *derivedOps) {
        auto writerId =
            addToWriterVector(opCtx, &op, writerVectors, collPropertiesCache, serialWriterId);
        if (serial && !serialWriterId) {
            serialWriterId.emplace(writerId);
        }
    }
}

void OplogApplierUtils::addDerivedPrepares(
    OperationContext* opCtx,
    OplogEntry* prepareOp,
    std::vector<OplogEntry>* derivedOps,
    std::vector<std::vector<ApplierOperation>>* writerVectors,
    CachedCollectionProperties* collPropertiesCache) {

    uint32_t bufSplits = 0;
    std::vector<std::vector<const OplogEntry*>> bufWriterVectors(writerVectors->size());

    // Add the ops in the prepared transaction to the buffered writer vectors.
    for (auto&& op : *derivedOps) {
        auto writerId = addToWriterVector(opCtx, &op, &bufWriterVectors, collPropertiesCache);
        bufSplits += bufWriterVectors[writerId].size() == 1;
    }

    // Create the split sessions and track them with the the session of this prepare entry.
    uint32_t realSplits = std::max<uint32_t>(bufSplits, 1);
    auto splitSessManager = ReplicationCoordinator::get(opCtx)->getSplitPrepareSessionManager();
    const auto& splitSessions = splitSessManager->splitSession(
        *prepareOp->getSessionId(), *prepareOp->getTxnNumber(), realSplits);
    invariant(splitSessions.size() == realSplits);

    // For empty (read-only) prepares, the namespace of the prepare oplog entry (admin.$cmd)
    // will be used to decide which writer vector to add to.
    if (!bufSplits) {
        auto writerId = getWriterId(opCtx, prepareOp, collPropertiesCache, writerVectors->size());
        (*writerVectors)[writerId].emplace_back(prepareOp,
                                                ApplicationInstruction::applySplitPrepareOps,
                                                splitSessions[0],
                                                std::vector<const OplogEntry*>{});
        return;
    }

    // For each writer thread that has been assigned ops for this transaction, acquire a
    // split session and transfer the ops to the real writer vector.
    for (size_t i = 0, j = 0; i < bufWriterVectors.size(); ++i) {
        auto& bufWriter = bufWriterVectors[i];
        auto& realWriter = (*writerVectors)[i];
        if (!bufWriter.empty()) {
            realWriter.emplace_back(prepareOp,
                                    ApplicationInstruction::applySplitPrepareOps,
                                    splitSessions[j++],
                                    std::move(bufWriter));
        }
    }
}

NamespaceString OplogApplierUtils::parseUUIDOrNs(OperationContext* opCtx,
                                                 const OplogEntry& oplogEntry) {
    auto optionalUuid = oplogEntry.getUuid();
    if (!optionalUuid) {
        return oplogEntry.getNss();
    }

    const auto& uuid = optionalUuid.value();
    auto catalog = CollectionCatalog::get(opCtx);
    auto nss = catalog->lookupNSSByUUID(opCtx, uuid);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "No namespace with UUID " << uuid.toString(),
            nss);
    return *nss;
}

NamespaceStringOrUUID OplogApplierUtils::getNsOrUUID(const NamespaceString& nss,
                                                     const OplogEntry& op) {
    if (auto ui = op.getUuid()) {
        return {nss.dbName(), ui.value()};
    }
    return nss;
}

Status OplogApplierUtils::applyOplogEntryOrGroupedInsertsCommon(
    OperationContext* opCtx,
    const OplogEntryOrGroupedInserts& entryOrGroupedInserts,
    OplogApplication::Mode oplogApplicationMode,
    const bool isDataConsistent,
    IncrementOpsAppliedStatsFn incrementOpsAppliedStats,
    OpCounters* opCounters) {
    invariant(DocumentValidationSettings::get(opCtx).isSchemaValidationDisabled());

    auto op = entryOrGroupedInserts.getOp();
    // Count each log op application as a separate operation, for reporting purposes
    CurOp individualOp(opCtx);
    const NamespaceString nss(op.getNss());
    auto opType = op.getOpType();

    if ((gMultitenancySupport && serverGlobalParams.featureCompatibility.isVersionInitialized() &&
         gFeatureFlagRequireTenantID.isEnabled(serverGlobalParams.featureCompatibility))) {
        invariant(op.getTid() == nss.tenantId());
    } else {
        invariant(op.getTid() == boost::none);
    }

    if (opType == OpTypeEnum::kNoop) {
        incrementOpsAppliedStats();
        return Status::OK();
    } else if (DurableOplogEntry::isCrudOpType(opType)) {
        auto status =
            writeConflictRetry(opCtx, "applyOplogEntryOrGroupedInserts_CRUD", nss.ns(), [&] {
                // Need to throw instead of returning a status for it to be properly ignored.
                try {
                    AutoGetCollection autoColl(opCtx,
                                               getNsOrUUID(nss, op),
                                               fixLockModeForSystemDotViewsChanges(nss, MODE_IX));
                    auto db = autoColl.getDb();
                    uassert(ErrorCodes::NamespaceNotFound,
                            str::stream() << "missing database (" << nss.db() << ")",
                            db);
                    OldClientContext ctx(opCtx, autoColl.getNss(), db);

                    // We convert updates to upserts in secondary mode when the
                    // oplogApplicationEnforcesSteadyStateConstraints parameter is false, to avoid
                    // failing on the constraint that updates in steady state mode always update
                    // an existing document.
                    //
                    // In initial sync and recovery modes we always ignore errors about missing
                    // documents on update, so there is no reason to convert the updates to upsert.

                    bool shouldAlwaysUpsert = !oplogApplicationEnforcesSteadyStateConstraints &&
                        oplogApplicationMode == OplogApplication::Mode::kSecondary;
                    Status status = applyOperation_inlock(opCtx,
                                                          db,
                                                          entryOrGroupedInserts,
                                                          shouldAlwaysUpsert,
                                                          oplogApplicationMode,
                                                          isDataConsistent,
                                                          incrementOpsAppliedStats);
                    if (!status.isOK() && status.code() == ErrorCodes::WriteConflict) {
                        throwWriteConflictException(
                            str::stream() << "WriteConflict caught when applying operation."
                                          << " Original error: " << status.reason());
                    }
                    return status;
                } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
                    // This can happen in initial sync or recovery modes (when a delete of the
                    // namespace appears later in the oplog), but we will ignore it in the caller.
                    //
                    // When we're not enforcing steady-state constraints, the error is ignored
                    // only for deletes, on the grounds that deleting from a non-existent collection
                    // is a no-op.
                    if (opType == OpTypeEnum::kDelete &&
                        !oplogApplicationEnforcesSteadyStateConstraints &&
                        oplogApplicationMode == OplogApplication::Mode::kSecondary) {
                        if (opCounters) {
                            opCounters->gotDeleteFromMissingNamespace();
                        }
                        return Status::OK();
                    }

                    ex.addContext(str::stream() << "Failed to apply operation: "
                                                << redact(entryOrGroupedInserts.toBSON()));
                    throw;
                }
            });
        return status;
    } else if (opType == OpTypeEnum::kCommand) {
        auto status =
            writeConflictRetry(opCtx, "applyOplogEntryOrGroupedInserts_command", nss.ns(), [&] {
                // A special case apply for commands to avoid implicit database creation.
                Status status = applyCommand_inlock(opCtx, op, oplogApplicationMode);
                incrementOpsAppliedStats();
                return status;
            });
        if (op.getCommandType() == mongo::repl::OplogEntry::CommandType::kDrop) {
            hangAfterApplyingCollectionDropOplogEntry.executeIf(
                [&](const BSONObj&) {
                    hangAfterApplyingCollectionDropOplogEntry.pauseWhileSet();
                    LOGV2(5863600,
                          "Hanging due to 'hangAfterApplyingCollectionDropOplogEntry' failpoint.");
                },
                [&](const BSONObj& data) { return (nss.db() == data["dbName"].str()); });
        }
        return status;
    }

    MONGO_UNREACHABLE;
}

Status OplogApplierUtils::applyOplogBatchCommon(
    OperationContext* opCtx,
    std::vector<ApplierOperation>* ops,
    OplogApplication::Mode oplogApplicationMode,
    bool allowNamespaceNotFoundErrorsOnCrudOps,
    const bool isDataConsistent,
    InsertGroup::ApplyFunc applyOplogEntryOrGroupedInserts) noexcept {

    // We cannot do document validation, because document validation could have been disabled when
    // these oplog entries were generated.
    DisableDocumentValidation validationDisabler(opCtx);
    // Group the operations by namespace in order to get larger groups for bulk inserts, but do not
    // mix up the current order of oplog entries within the same namespace (thus *stable* sort).
    stableSortByNamespace(ops);
    InsertGroup insertGroup(
        ops, opCtx, oplogApplicationMode, isDataConsistent, applyOplogEntryOrGroupedInserts);

    for (auto it = ops->cbegin(); it != ops->cend(); ++it) {
        const OplogEntry& entry = **it;

        // If we are successful in grouping and applying inserts, advance the current iterator
        // past the end of the inserted group of entries.
        auto groupResult = insertGroup.groupAndApplyInserts(it);
        if (groupResult.isOK()) {
            it = groupResult.getValue();
            continue;
        }

        // If we didn't create a group, try to apply the op individually.
        try {
            const Status status = applyOplogEntryOrGroupedInserts(
                opCtx, ApplierOperation{&entry}, oplogApplicationMode, isDataConsistent);

            if (!status.isOK()) {
                // Tried to apply an update operation but the document is missing, there must be
                // a delete operation for the document later in the oplog.
                if (status == ErrorCodes::UpdateOperationFailed &&
                    (oplogApplicationMode == OplogApplication::Mode::kInitialSync ||
                     oplogApplicationMode == OplogApplication::Mode::kRecovering)) {
                    continue;
                }

                LOGV2_FATAL_CONTINUE(21237,
                                     "Error applying operation ({oplogEntry}): {error}",
                                     "Error applying operation",
                                     "oplogEntry"_attr = redact(entry.toBSONForLogging()),
                                     "error"_attr = causedBy(redact(status)));
                return status;
            }
        } catch (const DBException& e) {
            // SERVER-24927 If we have a NamespaceNotFound exception, then this document will be
            // dropped before initial sync or recovery ends anyways and we should ignore it.
            if (e.code() == ErrorCodes::NamespaceNotFound && entry.isCrudOpType() &&
                allowNamespaceNotFoundErrorsOnCrudOps) {
                continue;
            }

            LOGV2_FATAL_CONTINUE(21238,
                                 "writer worker caught exception: {error} on: {oplogEntry}",
                                 "Writer worker caught exception",
                                 "error"_attr = redact(e),
                                 "oplogEntry"_attr = redact(entry.toBSONForLogging()));
            return e.toStatus();
        }
    }
    return Status::OK();
}

}  // namespace repl
}  // namespace mongo
