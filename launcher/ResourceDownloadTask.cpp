// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (c) 2022-2023 flowln <flowlnlnln@gmail.com>
 *  Copyright (C) 2022 Sefa Eyeoglu <contact@scrumplex.net>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "ResourceDownloadTask.h"

#include <utility>

#include "Application.h"

#include "FileSystem.h"
#include "MirrorUtils.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/mod/ResourceFolderModel.h"

#include "minecraft/mod/ShaderPackFolderModel.h"
#include "modplatform/ModIndex.h"
#include "modplatform/helpers/HashUtils.h"
#include "net/ApiRequest.h"
#include "net/ChecksumValidator.h"
#include "net/NetJob.h"

namespace {
Net::ModrinthDownloadMeta createModrinthMeta(MinecraftInstance* instance, QString reason, QString dependentOn)
{
    auto* profile = instance->getPackProfile();
    if (!profile) {
        return {};
    }

    auto loaders = profile->getModLoadersList();

    return { .reason = std::move(reason),
             .gameVersion = profile->getComponentVersion("net.minecraft"),
             .loader = !loaders.isEmpty() ? ModPlatform::getModLoaderAsString(loaders.first()) : "",
             .dependentOn = std::move(dependentOn) };
}
}  // namespace

ResourceDownloadTask::ResourceDownloadTask(ModPlatform::IndexedPack::Ptr pack,
                                           ModPlatform::IndexedVersion version,
                                           ResourceFolderModel* packs,
                                           bool isIndexed,
                                           QString downloadReason,
                                           QString dependentOn)
    : m_pack(std::move(pack)), m_pack_version(std::move(version)), m_pack_model(packs)
{
    if (isIndexed) {
        m_update_task.reset(new LocalResourceUpdateTask(m_pack_model->indexDir(), *m_pack, m_pack_version));
        connect(m_update_task.get(), &LocalResourceUpdateTask::hasOldResource, this, &ResourceDownloadTask::hasOldResource);

        addTask(m_update_task);
    }

    m_downloadTask = makeShared<MultipleOptionsTask>(tr("Resource download"));
    const auto meta = createModrinthMeta(m_pack_model->instance(), std::move(downloadReason), std::move(dependentOn));
    QList<QUrl> urls{ m_pack_version.downloadUrl };
    const auto mirrorUrl = MirrorUtils::modFallbackUrl(m_pack_version.downloadUrl,
                                                      APPLICATION->settings()->get("MirrorRootURL").toString());
    if (!mirrorUrl.isEmpty()) {
        urls.append(mirrorUrl);
    }

    for (const auto& url : urls) {
        auto job = makeShared<NetJob>(tr("Resource download"), APPLICATION->network());
        job->setStatus(tr("Downloading resource:\n%1").arg(url.toString()));
        // Try the mirror before asking the user to retry the failed download.
        job->setAskRetry(url == urls.last());
        auto action = Net::ApiRequest::makeFile(url, m_pack_model->dir().absoluteFilePath(getFilename()),
                                              Net::Request::Option::NoOptions, meta);
        if (!m_pack_version.hashType.isEmpty() && !m_pack_version.hash.isEmpty()) {
            switch (Hashing::algorithmFromString(m_pack_version.hashType)) {
                case Hashing::Algorithm::Md4:
                    action->addValidator(new Net::ChecksumValidator(QCryptographicHash::Algorithm::Md4, m_pack_version.hash));
                    break;
                case Hashing::Algorithm::Md5:
                    action->addValidator(new Net::ChecksumValidator(QCryptographicHash::Algorithm::Md5, m_pack_version.hash));
                    break;
                case Hashing::Algorithm::Sha1:
                    action->addValidator(new Net::ChecksumValidator(QCryptographicHash::Algorithm::Sha1, m_pack_version.hash));
                    break;
                case Hashing::Algorithm::Sha256:
                    action->addValidator(new Net::ChecksumValidator(QCryptographicHash::Algorithm::Sha256, m_pack_version.hash));
                    break;
                case Hashing::Algorithm::Sha512:
                    action->addValidator(new Net::ChecksumValidator(QCryptographicHash::Algorithm::Sha512, m_pack_version.hash));
                    break;
                default:
                    break;
            }
        }
        job->addNetAction(action);
        connect(job.get(), &NetJob::progress, this, &ResourceDownloadTask::downloadProgressChanged);
        connect(job.get(), &NetJob::stepProgress, this, &ResourceDownloadTask::propagateStepProgress);
        m_downloadTask->addTask(job);
    }
    connect(m_downloadTask.get(), &Task::succeeded, this, &ResourceDownloadTask::downloadSucceeded);
    connect(m_downloadTask.get(), &Task::failed, this, &ResourceDownloadTask::downloadFailed);

    addTask(m_downloadTask);
}

void ResourceDownloadTask::downloadSucceeded()
{
    m_downloadTask.reset();
    auto oldName = std::get<0>(to_delete);
    auto oldFilename = std::get<1>(to_delete);

    if (oldName.isEmpty() || oldFilename == m_pack_version.fileName) {
        return;
    }

    m_pack_model->uninstallResource(oldFilename, true);

    // also rename the shader config file
    if (dynamic_cast<ShaderPackFolderModel*>(m_pack_model) != nullptr) {
        QFileInfo oldConfig(m_pack_model->dir(), oldFilename + ".txt");
        QFileInfo newConfig(m_pack_model->dir(), getFilename() + ".txt");

        if (oldConfig.exists() && !newConfig.exists()) {
            bool success = FS::move(oldConfig.filePath(), newConfig.filePath());

            if (!success) {
                emit logWarning(tr("Failed to rename shader config from '%1' to '%2'").arg(oldConfig.fileName(), newConfig.fileName()));
            }
        }
    }
}

void ResourceDownloadTask::downloadFailed(QString reason)
{
    m_downloadTask.reset();
    emitFailed(std::move(reason));
}

void ResourceDownloadTask::downloadProgressChanged(qint64 current, qint64 total)
{
    emit progress(current, total);
}

// This indirection is done so that we don't delete a mod before being sure it was
// downloaded successfully!
void ResourceDownloadTask::hasOldResource(const QString& name, const QString& filename)
{
    to_delete = { name, filename };
}
