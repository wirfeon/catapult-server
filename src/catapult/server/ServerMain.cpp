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

#include "ServerMain.h"
#include "Signals.h"
#include "catapult/config/LocalNodeConfiguration.h"
#include "catapult/config/ValidateConfiguration.h"
#include "catapult/crypto/KeyPair.h"
#include "catapult/crypto/KeyUtils.h"
#include "catapult/io/FileLock.h"
#include "catapult/thread/ThreadInfo.h"
#include "catapult/utils/ExceptionLogging.h"
#include "catapult/utils/Logging.h"
#include "catapult/version/version.h"
#include <iostream>

namespace catapult { namespace server {

	namespace {
		config::LocalNodeConfiguration LoadConfiguration(int argc, const char** argv) {
			auto resourcesPath = GetResourcesPath(argc, argv);
			std::cout << "loading resources from " << resourcesPath << std::endl;
			return config::LocalNodeConfiguration::LoadFromPath(resourcesPath);
		}

		std::unique_ptr<utils::LogFilter> CreateLogFilter(const config::BasicLoggerConfiguration& config) {
			auto pFilter = std::make_unique<utils::LogFilter>(config.Level);
			for (const auto& pair : config.ComponentLevels)
				pFilter->setLevel(pair.first.c_str(), pair.second);

			return pFilter;
		}

		std::shared_ptr<void> SetupLogging(const config::LoggingConfiguration& config) {
			auto pBootstrapper = std::make_shared<utils::LoggingBootstrapper>();
			pBootstrapper->addConsoleLogger(config::GetConsoleLoggerOptions(config.Console), *CreateLogFilter(config.Console));
			pBootstrapper->addFileLogger(config::GetFileLoggerOptions(config.File), *CreateLogFilter(config.File));
			return pBootstrapper;
		}

		void Run(config::LocalNodeConfiguration&& config, const CreateLocalNodeFunc& createLocalNode) {
			auto keyPair = crypto::KeyPair::FromString(config.User.BootKey);

			CATAPULT_LOG(info) << "booting local node with public key " << crypto::FormatKey(keyPair.publicKey());
			auto pLocalNode = createLocalNode(std::move(config), keyPair);
			WaitForTerminationSignal();

			CATAPULT_LOG(info) << "shutting down local node";
			pLocalNode.reset();
		}
	}

	boost::filesystem::path GetResourcesPath(int argc, const char** argv) {
		return boost::filesystem::path(argc > 1 ? argv[1] : "..") / "resources";
	}

	int ServerMain(int argc, const char** argv, const CreateLocalNodeFunc& createLocalNode) {
		thread::SetThreadName("Server Main");
		version::WriteVersionInformation(std::cout);

		try {
			// 1. load and validate the configuration
			auto config = LoadConfiguration(argc, argv);
			ValidateConfiguration(config);

			// 2. initialize logging
			auto pLoggingGuard = SetupLogging(config.Logging);

			// 3. check instance
			boost::filesystem::path lockFilePath = config.User.DataDirectory;
			lockFilePath /= "file.lock";
			io::FileLock instanceLock(lockFilePath.generic_string());
			if (!instanceLock.try_lock()) {
				CATAPULT_LOG(fatal) << "could not acquire instance lock " << lockFilePath;
				return -3;
			}

			// 4. run the server
			try {
				Run(std::move(config), createLocalNode);
				return 0;
			} catch (...) {
				CATAPULT_LOG(fatal) << UNHANDLED_EXCEPTION_MESSAGE("running local node");
				return -2;
			}
		} catch (...) {
			std::cerr << UNHANDLED_EXCEPTION_MESSAGE("loading configuration");
			return -1;
		}
	}
}}
