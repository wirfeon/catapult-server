/**
*** Copyright (c) 2016-present,
*** Jaguar0625, gimre, BloodyRookie, Tech Bureau, Corp. All rights reserved.
***
*** This file is part of Catapult.
***
*** Catapult is free software: you can redistribute it and/or modify
*** it under the terms of the GNU Lesser General Public License as published by
*** the Free Software Foundation, either version 3 of the License, or
*** (at your option) any later version.
***
*** Catapult is distributed in the hope that it will be useful,
*** but WITHOUT ANY WARRANTY; without even the implied warranty of
*** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*** GNU Lesser General Public License for more details.
***
*** You should have received a copy of the GNU Lesser General Public License
*** along with Catapult. If not, see <http://www.gnu.org/licenses/>.
**/

#include "DispatcherService.h"
#include "ExecutionConfigurationFactory.h"
#include "PredicateUtils.h"
#include "RollbackInfo.h"
#include "catapult/cache/MemoryUtCache.h"
#include "catapult/cache/ReadOnlyCatapultCache.h"
#include "catapult/cache_core/AccountStateCache.h"
#include "catapult/cache_core/BlockDifficultyCache.h"
#include "catapult/cache_core/ImportanceView.h"
#include "catapult/chain/BlockExecutor.h"
#include "catapult/chain/BlockScorer.h"
#include "catapult/chain/ChainUtils.h"
#include "catapult/chain/UtUpdater.h"
#include "catapult/config/LocalNodeConfiguration.h"
#include "catapult/consumers/AuditConsumer.h"
#include "catapult/consumers/BlockConsumers.h"
#include "catapult/consumers/ReclaimMemoryInspector.h"
#include "catapult/consumers/TransactionConsumers.h"
#include "catapult/disruptor/BatchRangeDispatcher.h"
#include "catapult/extensions/DispatcherUtils.h"
#include "catapult/extensions/LocalNodeChainScore.h"
#include "catapult/extensions/PluginUtils.h"
#include "catapult/extensions/ServiceLocator.h"
#include "catapult/extensions/ServiceState.h"
#include "catapult/model/BlockChainConfiguration.h"
#include "catapult/plugins/PluginManager.h"
#include "catapult/subscribers/StateChangeSubscriber.h"
#include "catapult/subscribers/TransactionStatusSubscriber.h"
#include "catapult/thread/MultiServicePool.h"
#include "catapult/validators/AggregateEntityValidator.h"
#include <boost/filesystem.hpp>

using namespace catapult::consumers;
using namespace catapult::disruptor;

namespace catapult { namespace sync {

	namespace {
		// region utils

		ConsumerDispatcherOptions CreateBlockConsumerDispatcherOptions(const config::NodeConfiguration& config) {
			auto options = ConsumerDispatcherOptions("block dispatcher", config.BlockDisruptorSize);
			options.ElementTraceInterval = config.BlockElementTraceInterval;
			options.ShouldThrowIfFull = config.ShouldAbortWhenDispatcherIsFull;
			return options;
		}

		ConsumerDispatcherOptions CreateTransactionConsumerDispatcherOptions(const config::NodeConfiguration& config) {
			auto options = ConsumerDispatcherOptions("transaction dispatcher", config.TransactionDisruptorSize);
			options.ElementTraceInterval = config.TransactionElementTraceInterval;
			options.ShouldThrowIfFull = config.ShouldAbortWhenDispatcherIsFull;
			return options;
		}

		std::unique_ptr<ConsumerDispatcher> CreateConsumerDispatcher(
				extensions::ServiceState& state,
				const ConsumerDispatcherOptions& options,
				std::vector<DisruptorConsumer>&& disruptorConsumers) {
			auto& statusSubscriber = state.transactionStatusSubscriber();
			auto reclaimMemoryInspector = CreateReclaimMemoryInspector();
			auto inspector = [&statusSubscriber, reclaimMemoryInspector](auto& input, const auto& completionResult) {
				statusSubscriber.flush();
				reclaimMemoryInspector(input, completionResult);
			};

			// if enabled, add an audit consumer before all other consumers
			const auto& config = state.config();
			if (config.Node.ShouldAuditDispatcherInputs) {
				auto auditPath = boost::filesystem::path(config.User.DataDirectory) / "audit" / std::string(options.DispatcherName);
				auditPath /= std::to_string(state.timeSupplier()().unwrap());
				CATAPULT_LOG(debug) << "enabling auditing to " << auditPath;

				boost::filesystem::create_directories(auditPath);
				disruptorConsumers.insert(disruptorConsumers.begin(), CreateAuditConsumer(auditPath.generic_string()));
			}

			return std::make_unique<ConsumerDispatcher>(options, disruptorConsumers, inspector);
		}

		// endregion

		// region block

		BlockChainSyncHandlers::UndoBlockFunc CreateSyncUndoBlockHandler(
				const std::shared_ptr<const observers::EntityObserver>& pUndoObserver) {
			return [pUndoObserver](const auto& blockElement, const auto& state) {
				CATAPULT_LOG(debug) << "rolling back block at height " << blockElement.Block.Height;
				chain::RollbackBlock(blockElement, *pUndoObserver, state);
			};
		}

		BlockChainProcessor CreateSyncProcessor(
				const model::BlockChainConfiguration& blockChainConfig,
				const chain::ExecutionConfiguration& executionConfig) {
			return CreateBlockChainProcessor(
					[&blockChainConfig](const cache::ReadOnlyCatapultCache& cache) {
						cache::ImportanceView view(cache.sub<cache::AccountStateCache>());
						return chain::BlockHitPredicate(blockChainConfig, [view](const auto& publicKey, auto height) {
							return view.getAccountImportanceOrDefault(publicKey, height);
						});
					},
					chain::CreateBatchEntityProcessor(executionConfig));
		}

		BlockChainSyncHandlers CreateBlockChainSyncHandlers(extensions::ServiceState& state, RollbackInfo& rollbackInfo) {
			const auto& blockChainConfig = state.config().BlockChain;
			const auto& pluginManager = state.pluginManager();

			BlockChainSyncHandlers syncHandlers;
			syncHandlers.DifficultyChecker = [&rollbackInfo, blockChainConfig](const auto& blocks, const cache::CatapultCache& cache) {
				auto result = chain::CheckDifficulties(cache.sub<cache::BlockDifficultyCache>(), blocks, blockChainConfig);
				rollbackInfo.reset();
				return blocks.size() == result;
			};

			auto undoBlockHandler = CreateSyncUndoBlockHandler(extensions::CreateUndoEntityObserver(pluginManager));
			syncHandlers.UndoBlock = [&rollbackInfo, undoBlockHandler](const auto& blockElement, const auto& observerState) {
				rollbackInfo.increment();
				undoBlockHandler(blockElement, observerState);
			};
			syncHandlers.Processor = CreateSyncProcessor(blockChainConfig, CreateExecutionConfiguration(pluginManager));

			syncHandlers.StateChange = [&rollbackInfo, &localScore = state.score(), &subscriber = state.stateChangeSubscriber()](
					const auto& changeInfo) {
				localScore += changeInfo.ScoreDelta;

				// note: changeInfo contains only score delta, subscriber will get both current local score and changeInfo
				subscriber.notifyScoreChange(localScore.get());
				subscriber.notifyStateChange(changeInfo);

				rollbackInfo.save();
			};

			syncHandlers.TransactionsChange = state.hooks().transactionsChangeHandler();
			return syncHandlers;
		}

		class BlockDispatcherBuilder {
		public:
			explicit BlockDispatcherBuilder(extensions::ServiceState& state)
					: m_state(state)
					, m_nodeConfig(m_state.config().Node)
			{}

		public:
			void addHashConsumers() {
				m_consumers.push_back(CreateBlockHashCalculatorConsumer(m_state.pluginManager().transactionRegistry()));
				m_consumers.push_back(CreateBlockHashCheckConsumer(
					m_state.timeSupplier(),
					extensions::CreateHashCheckOptions(m_nodeConfig.ShortLivedCacheBlockDuration, m_nodeConfig)));
			}

			void addPrecomputedTransactionAddressConsumer(const model::NotificationPublisher& publisher) {
				m_consumers.push_back(CreateBlockAddressExtractionConsumer(publisher));
			}

			std::shared_ptr<ConsumerDispatcher> build(
					const std::shared_ptr<thread::IoServiceThreadPool>& pValidatorPool,
					RollbackInfo& rollbackInfo) {
				m_consumers.push_back(CreateBlockChainCheckConsumer(
						m_nodeConfig.MaxBlocksPerSyncAttempt,
						m_state.config().BlockChain.MaxBlockFutureTime,
						m_state.timeSupplier()));
				m_consumers.push_back(CreateBlockStatelessValidationConsumer(
						extensions::CreateStatelessValidator(m_state.pluginManager()),
						validators::CreateParallelValidationPolicy(pValidatorPool),
						ToUnknownTransactionPredicate(m_state.hooks().knownHashPredicate(m_state.utCache()))));

				auto disruptorConsumers = DisruptorConsumersFromBlockConsumers(m_consumers);
				disruptorConsumers.push_back(CreateBlockChainSyncConsumer(
						m_state.cache(),
						m_state.state(),
						m_state.storage(),
						m_state.config().BlockChain.MaxRollbackBlocks,
						CreateBlockChainSyncHandlers(m_state, rollbackInfo)));

				disruptorConsumers.push_back(CreateNewBlockConsumer(m_state.hooks().newBlockSink(), InputSource::Local));
				return CreateConsumerDispatcher(
						m_state,
						CreateBlockConsumerDispatcherOptions(m_nodeConfig),
						std::move(disruptorConsumers));
			}

		private:
			extensions::ServiceState& m_state;
			const config::NodeConfiguration& m_nodeConfig;
			std::vector<BlockConsumer> m_consumers;
		};

		void RegisterBlockDispatcherService(
				const std::shared_ptr<ConsumerDispatcher>& pDispatcher,
				thread::MultiServicePool::ServiceGroup& serviceGroup,
				extensions::ServiceLocator& locator,
				extensions::ServiceState& state) {
			serviceGroup.registerService(pDispatcher);
			locator.registerService("dispatcher.block", pDispatcher);

			state.hooks().setBlockRangeConsumerFactory([&dispatcher = *pDispatcher](auto source) {
				return [&dispatcher, source](auto&& range) {
					dispatcher.processElement(ConsumerInput(std::move(range), source));
				};
			});

			state.hooks().setCompletionAwareBlockRangeConsumerFactory([&dispatcher = *pDispatcher](auto source) {
				return [&dispatcher, source](auto&& range, const auto& processingComplete) {
					return dispatcher.processElement(ConsumerInput(std::move(range), source), processingComplete);
				};
			});
		}

		// endregion

		// region transaction

		class TransactionDispatcherBuilder {
		public:
			explicit TransactionDispatcherBuilder(extensions::ServiceState& state)
					: m_state(state)
					, m_nodeConfig(m_state.config().Node)
			{}

		public:
			void addHashConsumers() {
				m_consumers.push_back(CreateTransactionHashCalculatorConsumer(m_state.pluginManager().transactionRegistry()));
				m_consumers.push_back(CreateTransactionHashCheckConsumer(
						m_state.timeSupplier(),
						extensions::CreateHashCheckOptions(m_nodeConfig.ShortLivedCacheTransactionDuration, m_nodeConfig),
						m_state.hooks().knownHashPredicate(m_state.utCache())));
			}

			void addPrecomputedTransactionAddressConsumer(const model::NotificationPublisher& publisher) {
				m_consumers.push_back(CreateTransactionAddressExtractionConsumer(publisher));
			}

			std::shared_ptr<ConsumerDispatcher> build(
					const std::shared_ptr<thread::IoServiceThreadPool>& pValidatorPool,
					chain::UtUpdater& utUpdater) {
				m_consumers.push_back(CreateTransactionStatelessValidationConsumer(
						extensions::CreateStatelessValidator(m_state.pluginManager()),
						validators::CreateParallelValidationPolicy(pValidatorPool),
						extensions::SubscriberToSink(m_state.transactionStatusSubscriber())));

				auto disruptorConsumers = DisruptorConsumersFromTransactionConsumers(m_consumers);
				disruptorConsumers.push_back(CreateNewTransactionsConsumer(
						[&utUpdater, newTransactionsSink = m_state.hooks().newTransactionsSink()](auto&& transactionInfos) {
					/// Note that all transaction infos are broadcast even though some transactions might fail stateful validation because:
					/// 1. even though a transaction can fail stateful validation on one node, it might pass the validation on another
					/// 2. if the node is not synced it might reject many transactions that are perfectly valid due to missing account
					///    state information
					newTransactionsSink(transactionInfos);
					utUpdater.update(std::move(transactionInfos));
				}));

				return CreateConsumerDispatcher(
						m_state,
						CreateTransactionConsumerDispatcherOptions(m_nodeConfig),
						std::move(disruptorConsumers));
			}

		private:
			extensions::ServiceState& m_state;
			const config::NodeConfiguration& m_nodeConfig;
			std::vector<TransactionConsumer> m_consumers;
		};

		void RegisterTransactionDispatcherService(
				const std::shared_ptr<ConsumerDispatcher>& pDispatcher,
				thread::MultiServicePool::ServiceGroup& serviceGroup,
				extensions::ServiceLocator& locator,
				extensions::ServiceState& state) {
			serviceGroup.registerService(pDispatcher);
			locator.registerService("dispatcher.transaction", pDispatcher);

			auto pBatchRangeDispatcher = std::make_shared<extensions::TransactionBatchRangeDispatcher>(*pDispatcher);
			locator.registerRootedService("dispatcher.transaction.batch", pBatchRangeDispatcher);

			state.hooks().setTransactionRangeConsumerFactory([&dispatcher = *pBatchRangeDispatcher](auto source) {
				return [&dispatcher, source](auto&& range) {
					dispatcher.queue(std::move(range), source);
				};
			});

			state.tasks().push_back(extensions::CreateBatchTransactionTask(*pBatchRangeDispatcher, "transaction"));
		}

		// endregion

		chain::UtUpdater& CreateAndRegisterUtUpdater(extensions::ServiceLocator& locator, extensions::ServiceState& state) {
			auto pUtUpdater = std::make_shared<chain::UtUpdater>(
					state.utCache(),
					state.cache(),
					CreateExecutionConfiguration(state.pluginManager()),
					state.timeSupplier(),
					extensions::SubscriberToSink(state.transactionStatusSubscriber()),
					CreateUtUpdaterThrottle(state.config()));
			locator.registerRootedService("dispatcher.utUpdater", pUtUpdater);

			auto& utUpdater = *pUtUpdater;
			state.hooks().addTransactionsChangeHandler([&utUpdater](const auto& changeInfo) {
				utUpdater.update(changeInfo.AddedTransactionHashes, changeInfo.RevertedTransactionInfos);
			});

			return utUpdater;
		}

		auto CreateAndRegisterRollbackService(
				extensions::ServiceLocator& locator,
				const chain::TimeSupplier& timeSupplier,
				const model::BlockChainConfiguration& config) {
			auto rollbackDurationFull = CalculateFullRollbackDuration(config);
			auto rollbackDurationHalf = utils::TimeSpan::FromMilliseconds(rollbackDurationFull.millis() / 2);
			auto pRollbackInfo = std::make_shared<RollbackInfo>(timeSupplier, rollbackDurationHalf);
			locator.registerRootedService("rollbacks", pRollbackInfo);
			return pRollbackInfo;
		}

		void AddRollbackCounter(
				extensions::ServiceLocator& locator,
				const std::string& counterName,
				RollbackResult rollbackResult,
				RollbackCounterType rollbackCounterType) {
			locator.registerServiceCounter<RollbackInfo>("rollbacks", counterName, [rollbackResult, rollbackCounterType](
					const auto& rollbackInfo) {
				return rollbackInfo.counter(rollbackResult, rollbackCounterType);
			});
		}

		class DispatcherServiceRegistrar : public extensions::ServiceRegistrar {
		public:
			extensions::ServiceRegistrarInfo info() const override {
				return { "Dispatcher", extensions::ServiceRegistrarPhase::Post_Remote_Peers };
			}

			void registerServiceCounters(extensions::ServiceLocator& locator) override {
				extensions::AddDispatcherCounters(locator, "dispatcher.block", "BLK");
				extensions::AddDispatcherCounters(locator, "dispatcher.transaction", "TX");

				AddRollbackCounter(locator, "RB COMMIT ALL", RollbackResult::Committed, RollbackCounterType::All);
				AddRollbackCounter(locator, "RB COMMIT RCT", RollbackResult::Committed, RollbackCounterType::Recent);
				AddRollbackCounter(locator, "RB IGNORE ALL", RollbackResult::Ignored, RollbackCounterType::All);
				AddRollbackCounter(locator, "RB IGNORE RCT", RollbackResult::Ignored, RollbackCounterType::Recent);
			}

			void registerServices(extensions::ServiceLocator& locator, extensions::ServiceState& state) override {
				// create shared services
				auto pValidatorPool = state.pool().pushIsolatedPool("validator");
				auto& utUpdater = CreateAndRegisterUtUpdater(locator, state);

				// create the block and transaction dispatchers and related services
				// (notice that the dispatcher service group must be after the validator isolated pool in order to allow proper shutdown)
				auto pServiceGroup = state.pool().pushServiceGroup("dispatcher service");

				BlockDispatcherBuilder blockDispatcherBuilder(state);
				blockDispatcherBuilder.addHashConsumers();

				TransactionDispatcherBuilder transactionDispatcherBuilder(state);
				transactionDispatcherBuilder.addHashConsumers();

				if (state.config().Node.ShouldPrecomputeTransactionAddresses) {
					auto pPublisher = state.pluginManager().createNotificationPublisher();
					blockDispatcherBuilder.addPrecomputedTransactionAddressConsumer(*pPublisher);
					transactionDispatcherBuilder.addPrecomputedTransactionAddressConsumer(*pPublisher);
					locator.registerRootedService("dispatcher.notificationPublisher", std::move(pPublisher));
				}

				auto pRollbackInfo = CreateAndRegisterRollbackService(locator, state.timeSupplier(), state.config().BlockChain);
				auto pBlockDispatcher = blockDispatcherBuilder.build(pValidatorPool, *pRollbackInfo);
				RegisterBlockDispatcherService(pBlockDispatcher, *pServiceGroup, locator, state);

				auto pTransactionDispatcher = transactionDispatcherBuilder.build(pValidatorPool, utUpdater);
				RegisterTransactionDispatcherService(pTransactionDispatcher, *pServiceGroup, locator, state);
			}
		};
	}

	DECLARE_SERVICE_REGISTRAR(Dispatcher)() {
		return std::make_unique<DispatcherServiceRegistrar>();
	}
}}
