#include <stdafx.h>

#include "fetcher.h"

#include <artwork/musicbrainz_fetcher.h>
#include <artwork/uploader.h>
#include <discord/discord_integration.h>

#include <component_paths.h>

#include <cpr/cpr.h>
#include <qwr/abort_callback.h>
#include <qwr/algorithm.h>
#include <qwr/file_helpers.h>
#include <qwr/thread_name_setter.h>
#include <qwr/visitor.h>

#include <unordered_map>

namespace fs = std::filesystem;

namespace
{

const std::chrono::milliseconds kRequestProcessingDelay{ 300 };
const std::chrono::seconds kFailureCooldown{ 45 };

std::unordered_map<qwr::u8string, std::chrono::steady_clock::time_point> g_failedCooldownMap;

bool IsValidHttpUrl( const std::optional<qwr::u8string>& urlOpt )
{
    return urlOpt.has_value() && urlOpt->starts_with( "https://" );
}

bool IsPermanentUrl( const std::optional<qwr::u8string>& urlOpt )
{
    return IsValidHttpUrl( urlOpt ) && urlOpt->find( "litter.catbox.moe" ) == qwr::u8string::npos;
}

qwr::u8string GenerateMusicBrainzArtPinId( const qwr::u8string& artist, const qwr::u8string& album )
{
    return artist + "|" + album;
}

std::optional<qwr::u8string> GenerateArtPinId( const drp::ArtworkFetcher::FetchRequest& request )
{
    const auto artPinId = std::visit(
        qwr::Visitor{
            []( const drp::ArtworkFetcher::MusicBrainzFetchRequest& req ) {
                const auto& [artist, album, userReleaseMbidOpt] = req;
                if ( artist.empty() || album.empty() )
                {
                    return qwr::u8string{};
                }
                return GenerateMusicBrainzArtPinId( req.artist, req.album );
            },
            []( const drp::ArtworkFetcher::UploadRequest& req ) {
                return req.artPinId;
            } },
        request );
    return ( artPinId.empty() ? std::optional<qwr::u8string>{} : artPinId );
}

bool IsRequestExecutable( const drp::ArtworkFetcher::FetchRequest& request )
{
    return std::visit(
        qwr::Visitor{
            []( const drp::ArtworkFetcher::MusicBrainzFetchRequest& /*req*/ ) {
                return true;
            },
            []( const drp::ArtworkFetcher::UploadRequest& req ) {
                return !req.uploadCommand.empty();
            } },
        request );
}

} // namespace

namespace drp
{

drp::ArtworkFetcher& ArtworkFetcher::Get()
{
    static ArtworkFetcher instance;
    return instance;
}

void ArtworkFetcher::Initialize()
{
    LoadCache();
    StartThread();
}

void ArtworkFetcher::Finalize()
{
    StopThread();
    SaveCache();
}

std::optional<qwr::u8string> ArtworkFetcher::GetArtUrl( const FetchRequest& request )
{
    const auto artPinIdOpt = GenerateArtPinId( request );
    if ( !artPinIdOpt )
    {
        return std::nullopt;
    }

    {
        std::unique_lock lock( mutex_ );

        if ( const auto it = artPinIdToArtUrl_.find( *artPinIdOpt );
             it != artPinIdToArtUrl_.end() && IsValidHttpUrl( it->second ) )
        {
            return it->second;
        }

        if ( !IsRequestExecutable( request ) )
        {
            return std::nullopt;
        }

        if ( const auto failIt = g_failedCooldownMap.find( *artPinIdOpt );
             failIt != g_failedCooldownMap.end() )
        {
            if ( std::chrono::steady_clock::now() < failIt->second )
            {
                return std::nullopt;
            }
            g_failedCooldownMap.erase( failIt );
        }

        currentRequestOpt_ = request;
        cv_.notify_all();
    }

    return std::nullopt;
}

void ArtworkFetcher::LoadCache( bool throwOnError )
{
    using json = nlohmann::json;

    try
    {
        const auto cachePath = GetCacheFilePath();
        if ( !fs::exists( cachePath ) )
        {
            return;
        }

        const auto content = qwr::file::ReadFile( cachePath, CP_UTF8 );
        decltype( artPinIdToArtUrl_ ) loadedMap;
        json::parse( content ).get_to( loadedMap );

        std::unique_lock lock( mutex_ );
        artPinIdToArtUrl_.clear();
        g_failedCooldownMap.clear();

        for ( auto& [key, valOpt] : loadedMap )
        {
            if ( !key.empty() && IsPermanentUrl( valOpt ) )
            {
                artPinIdToArtUrl_.emplace( std::move( key ), std::move( valOpt ) );
            }
        }
    }
    catch ( const qwr::QwrException& e )
    {
        LogError( fmt::format( "Failed to load cache: {}", e.what() ) );
        if ( throwOnError )
        {
            throw;
        }
    }
    catch ( const json::exception& e )
    {
        LogError( fmt::format( "Failed to load cache: {}", e.what() ) );
        if ( throwOnError )
        {
            throw;
        }
    }
    catch ( const fs::filesystem_error& e )
    {
        LogError( fmt::format( "Failed to load cache: {}", e.what() ) );
        if ( throwOnError )
        {
            throw;
        }
    }
}

void ArtworkFetcher::SaveCache()
{
    using json = nlohmann::json;

    try
    {
        decltype( artPinIdToArtUrl_ ) cleanSnapshot;
        {
            std::unique_lock lock( mutex_ );
            for ( const auto& [key, valOpt] : artPinIdToArtUrl_ )
            {
                if ( !key.empty() && IsPermanentUrl( valOpt ) )
                {
                    cleanSnapshot.emplace( key, valOpt );
                }
            }
        }

        const auto cachePath = GetCacheFilePath();
        fs::create_directories( cachePath.parent_path() );

        const auto content = json( cleanSnapshot ).dump( 2 );
        qwr::file::WriteFile( cachePath, content, false );
    }
    catch ( const qwr::QwrException& e )
    {
        LogError( fmt::format( "Failed to save cache: {}", e.what() ) );
    }
    catch ( const json::exception& e )
    {
        LogError( fmt::format( "Failed to save cache: {}", e.what() ) );
    }
    catch ( const fs::filesystem_error& e )
    {
        LogError( fmt::format( "Failed to save cache: {}", e.what() ) );
    }
}

std::filesystem::path ArtworkFetcher::GetCacheFilePath()
{
    static const auto cachePath = drp::path::ImageDir() / "art_urls.v2.0.1.json";
    return cachePath;
}

void ArtworkFetcher::StartThread()
{
    pThread_ = std::make_unique<std::jthread>( &ArtworkFetcher::ThreadMain, this );
    qwr::SetThreadName( *pThread_, "DRP ArtFetcher" );
}

void ArtworkFetcher::StopThread()
{
    if ( pThread_ )
    {
        pThread_->request_stop();
        cv_.notify_all();
        pThread_.reset();
    }
}

void ArtworkFetcher::ThreadMain()
{
    auto token = pThread_->get_stop_token();
    std::optional<FetchRequest> lastRequest;

    while ( !token.stop_requested() )
    {
        {
            std::unique_lock lock( mutex_ );
            cv_.wait_for( lock, kRequestProcessingDelay, [&] {
                return token.stop_requested() || lastRequest != currentRequestOpt_;
            } );

            if ( token.stop_requested() )
            {
                return;
            }

            if ( currentRequestOpt_ )
            {
                if ( const auto artPinIdOpt = GenerateArtPinId( *currentRequestOpt_ );
                     artPinIdOpt && artPinIdToArtUrl_.contains( *artPinIdOpt ) )
                {
                    currentRequestOpt_.reset();
                    lastRequest.reset();
                    continue;
                }
            }

            if ( lastRequest != currentRequestOpt_ )
            {
                lastRequest = currentRequestOpt_;
                continue;
            }

            if ( !lastRequest )
            {
                continue;
            }
        }

        auto artUrlOpt = std::visit( [&]( const auto& arg ) { return ProcessFetchRequest( arg ); }, *lastRequest );
        if ( !artUrlOpt && ( qwr::GlobalAbortCallback::GetInstance().is_aborting() || token.stop_requested() ) )
        {
            return;
        }

        if ( artUrlOpt && qwr::unicode::ToWide( *artUrlOpt ).length() > 254 )
        {
            LogError( fmt::format( "Failed to process art url `{}`:\nlength is bigger than 254", *artUrlOpt ) );
            artUrlOpt.reset();
        }

        const auto artPinIdOpt = GenerateArtPinId( *lastRequest );
        assert( artPinIdOpt );

        bool shouldPersistCache = false;
        {
            std::unique_lock lock( mutex_ );

            if ( IsValidHttpUrl( artUrlOpt ) )
            {
                artPinIdToArtUrl_.insert_or_assign( *artPinIdOpt, artUrlOpt );
                g_failedCooldownMap.erase( *artPinIdOpt );
                shouldPersistCache = IsPermanentUrl( artUrlOpt );
            }
            else
            {
                g_failedCooldownMap[ *artPinIdOpt ] = std::chrono::steady_clock::now() + kFailureCooldown;
            }

            if ( lastRequest == currentRequestOpt_ )
            {
                currentRequestOpt_.reset();

                if ( IsValidHttpUrl( artUrlOpt ) )
                {
                    fb2k::inMainThread( [] {
                        auto pm = DiscordAdapter::GetInstance().GetPresenceModifier();
                        pm.UpdateImage();
                    } );
                }
            }
            lastRequest.reset();
        }

        if ( shouldPersistCache )
        {
            SaveCache();
        }
    }
}

std::optional<qwr::u8string> ArtworkFetcher::ProcessFetchRequest( const MusicBrainzFetchRequest& request )
{
    try
    {
        return musicbrainz::FetchArt( request.artist, request.album, request.userReleaseMbidOpt );
    }
    catch ( const qwr::QwrException& e )
    {
        LogError( e.what() );
        return std::nullopt;
    }
    catch ( const exception_aborted& /*e*/ )
    {
        return std::nullopt;
    }
}

std::optional<qwr::u8string> ArtworkFetcher::ProcessFetchRequest( const UploadRequest& request )
{
    try
    {
        return UploadArt( request.handle, request.uploadCommand );
    }
    catch ( const qwr::QwrException& e )
    {
        LogError( e.what() );
        return std::nullopt;
    }
    catch ( const exception_aborted& /*e*/ )
    {
        return std::nullopt;
    }
}

} // namespace drp