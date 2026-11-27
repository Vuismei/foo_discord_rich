#include <stdafx.h>

#include "uploader.h"

#include <utils/subprocess_executor.h>

#include <component_paths.h>

#if __has_include( <nlohmann/json.hpp> )
#    include <nlohmann/json.hpp>
#endif

#include <cpr/cpr.h>
#include <qwr/abort_callback.h>
#include <qwr/final_action.h>
#include <qwr/winapi_error_helpers.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <vector>

#pragma comment( lib, "windowscodecs.lib" )
#pragma comment( lib, "ole32.lib" )

using Microsoft::WRL::ComPtr;

namespace
{

std::chrono::seconds kMaxWaitTime{ 10 };
constexpr UINT kMaxCoverDimension = 300;

struct ArtData
{
    album_art_data_ptr pArtData;
    std::optional<qwr::u8string> pathOpt;
};

const qwr::u8string& GetTempImageFilePathTemplate()
{
    static const qwr::u8string tmpImagePath{ ( drp::path::ImageDir() / "tmp_image." ).u8string() };
    return tmpImagePath;
}

std::optional<ArtData> GetArtData( const metadb_handle_ptr& handle, abort_callback& aborter )
{
    const auto pArtMgr = album_art_manager_v3::get();

    const auto handles = pfc::list_single_ref_t<metadb_handle_ptr>( handle );
    const auto artTypeGuid = album_art_ids::cover_front;
    const auto guids = pfc::list_single_ref_t<GUID>( artTypeGuid );

    try
    {
        const auto pArtExtractor = pArtMgr->open_v3( handles, guids, nullptr, aborter );
        if ( !pArtExtractor.is_valid() )
        {
            return std::nullopt;
        }

        const auto pArt = pArtExtractor->query( artTypeGuid, aborter );
        const auto pathOpt = [&]() -> std::optional<qwr::u8string> {
            const auto pPathList = pArtExtractor->query_paths( artTypeGuid, aborter );
            if ( !pPathList->get_count() )
            {
                return std::nullopt;
            }

            qwr::u8string path = pPathList->get_path( 0 );
            if ( path == handle->get_location().get_path() )
            {
                return std::nullopt;
            }

            if ( constexpr qwr::u8string_view pathPrefix = "file://";
                 path.starts_with( pathPrefix ) )
            {
                path = path.substr( pathPrefix.size() );
            }

            return path;
        }();

        return ArtData{ pArt, pathOpt };
    }
    catch ( const exception_album_art_not_found& /*e*/ )
    {
        return std::nullopt;
    }
}

std::vector<uint8_t> ResizeCoverToJpegInMemory( const void* srcData, size_t srcSize, UINT maxDimension = kMaxCoverDimension )
{
    if ( !srcData || srcSize == 0 )
    {
        return {};
    }

    const HRESULT hrCom = CoInitializeEx( nullptr, COINIT_MULTITHREADED );
    qwr::final_action cleanupCom( [&] {
        if ( SUCCEEDED( hrCom ) )
        {
            CoUninitialize();
        }
    } );

    ComPtr<IWICImagingFactory> pFactory;
    if ( FAILED( CoCreateInstance( CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS( &pFactory ) ) ) )
    {
        return {};
    }

    ComPtr<IWICStream> pInStream;
    if ( FAILED( pFactory->CreateStream( &pInStream ) ) )
    {
        return {};
    }
    if ( FAILED( pInStream->InitializeFromMemory( reinterpret_cast<WICInProcPointer>( const_cast<void*>( srcData ) ), static_cast<DWORD>( srcSize ) ) ) )
    {
        return {};
    }

    ComPtr<IWICBitmapDecoder> pDecoder;
    if ( FAILED( pFactory->CreateDecoderFromStream( pInStream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &pDecoder ) ) )
    {
        return {};
    }

    ComPtr<IWICBitmapFrameDecode> pFrame;
    if ( FAILED( pDecoder->GetFrame( 0, &pFrame ) ) )
    {
        return {};
    }

    UINT origW = 0;
    UINT origH = 0;
    if ( FAILED( pFrame->GetSize( &origW, &origH ) ) || origW == 0 || origH == 0 )
    {
        return {};
    }

    UINT targetW = origW;
    UINT targetH = origH;
    if ( origW > maxDimension || origH > maxDimension )
    {
        if ( origW >= origH )
        {
            targetW = maxDimension;
            targetH = ( std::max )( 1u, ( origH * maxDimension ) / origW );
        }
        else
        {
            targetH = maxDimension;
            targetW = ( std::max )( 1u, ( origW * maxDimension ) / origH );
        }
    }

    ComPtr<IWICBitmapScaler> pScaler;
    if ( FAILED( pFactory->CreateBitmapScaler( &pScaler ) ) )
    {
        return {};
    }
    if ( FAILED( pScaler->Initialize( pFrame.Get(), targetW, targetH, WICBitmapInterpolationModeFant ) ) )
    {
        return {};
    }

    ComPtr<IStream> pOutMem;
    if ( FAILED( CreateStreamOnHGlobal( nullptr, TRUE, &pOutMem ) ) )
    {
        return {};
    }

    ComPtr<IWICBitmapEncoder> pEncoder;
    if ( FAILED( pFactory->CreateEncoder( GUID_ContainerFormatJpeg, nullptr, &pEncoder ) ) )
    {
        return {};
    }
    if ( FAILED( pEncoder->Initialize( pOutMem.Get(), WICBitmapEncoderNoCache ) ) )
    {
        return {};
    }

    ComPtr<IWICBitmapFrameEncode> pOutFrame;
    ComPtr<IPropertyBag2> pProps;
    if ( FAILED( pEncoder->CreateNewFrame( &pOutFrame, &pProps ) ) )
    {
        return {};
    }

    PROPBAG2 option{};
    option.pstrName = const_cast<LPOLESTR>( L"ImageQuality" );
    VARIANT varVal;
    VariantInit( &varVal );
    varVal.vt = VT_R4;
    varVal.fltVal = 0.82f;
    pProps->Write( 1, &option, &varVal );
    VariantClear( &varVal );

    if ( FAILED( pOutFrame->Initialize( pProps.Get() ) ) )
    {
        return {};
    }
    if ( FAILED( pOutFrame->SetSize( targetW, targetH ) ) )
    {
        return {};
    }

    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if ( FAILED( pOutFrame->SetPixelFormat( &format ) ) )
    {
        return {};
    }
    if ( FAILED( pOutFrame->WriteSource( pScaler.Get(), nullptr ) ) )
    {
        return {};
    }

    if ( FAILED( pOutFrame->Commit() ) || FAILED( pEncoder->Commit() ) )
    {
        return {};
    }

    STATSTG stat{};
    if ( FAILED( pOutMem->Stat( &stat, STATFLAG_NONAME ) ) || stat.cbSize.LowPart == 0 )
    {
        return {};
    }

    std::vector<uint8_t> result( stat.cbSize.LowPart );
    LARGE_INTEGER zero{};
    pOutMem->Seek( zero, STREAM_SEEK_SET, nullptr );
    ULONG bytesRead = 0;
    if ( FAILED( pOutMem->Read( result.data(), stat.cbSize.LowPart, &bytesRead ) ) )
    {
        return {};
    }
    result.resize( bytesRead );
    return result;
}

qwr::u8string SaveBufferToTempJpeg( const std::vector<uint8_t>& jpegBytes, abort_callback& aborter )
{
    const auto imagePath = GetTempImageFilePathTemplate() + "jpg";
    try
    {
        service_ptr_t<file> file_ptr;
        filesystem::g_open_write_new( file_ptr, imagePath.c_str(), aborter );
        file_ptr->write( jpegBytes.data(), jpegBytes.size(), aborter );
    }
    catch ( const pfc::exception& e )
    {
        throw qwr::QwrException( "Failed to save temporary image file: ", e.what() );
    }
    return imagePath;
}

bool IsBuiltinCatboxCommand( const qwr::u8string& cmd )
{
    qwr::u8string lower;
    lower.reserve( cmd.size() );
    for ( unsigned char ch : cmd )
    {
        lower.push_back( static_cast<char>( std::tolower( ch ) ) );
    }
    return lower == "catbox" || lower == "builtin" || lower.find( "catbox" ) != qwr::u8string::npos;
}

qwr::u8string TrimWhitespace( qwr::u8string str )
{
    while ( !str.empty() && ( str.back() == '\r' || str.back() == '\n' || str.back() == ' ' || str.back() == '\t' ) )
    {
        str.pop_back();
    }
    return str;
}

qwr::u8string UploadWithFallback( const std::vector<uint8_t>& jpegBytes, abort_callback& aborter )
{
    aborter.check();

    const cpr::Header browserHeaders{
        { "User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36" },
        { "Expect", "" } };

    // 1. Ưu tiên Freeimage.host (trả về link CDN https://iili.io/... tương thích 100% proxy Discord)
    const auto resCdn = cpr::Post(
        cpr::Url{ "https://freeimage.host/api/1/upload" },
        browserHeaders,
        cpr::Multipart{
            { "key", "6d207e02198a847aa98d0a2a901485a5" },
            { "action", "upload" },
            { "format", "json" },
            { "source", cpr::Buffer{ jpegBytes.begin(), jpegBytes.end(), "cover.jpg" } } },
        cpr::ConnectTimeout{ 4000 },
        cpr::Timeout{ 7000 } );

    aborter.check();

    if ( resCdn.error.code == cpr::ErrorCode::OK && resCdn.status_code == 200 )
    {
        const auto j = nlohmann::json::parse( resCdn.text, nullptr, false );
        if ( !j.is_discarded() && j.contains( "image" ) && j[ "image" ].contains( "url" ) && j[ "image" ][ "url" ].is_string() )
        {
            const auto url = TrimWhitespace( j[ "image" ][ "url" ].get<qwr::u8string>() );
            if ( url.starts_with( "https://" ) )
            {
                return url;
            }
        }
    }

    // 2. Dự phòng sang Catbox
    const auto resCatbox = cpr::Post(
        cpr::Url{ "https://catbox.moe/user/api.php" },
        browserHeaders,
        cpr::Multipart{
            { "reqtype", "fileupload" },
            { "fileToUpload", cpr::Buffer{ jpegBytes.begin(), jpegBytes.end(), "cover.jpg" } } },
        cpr::ConnectTimeout{ 4000 },
        cpr::Timeout{ 7000 } );

    aborter.check();

    qwr::QwrException::ExpectTrue(
        resCatbox.error.code == cpr::ErrorCode::OK && resCatbox.status_code == 200,
        "All image hosts failed (CDN HTTP {}, Catbox HTTP {}): {}",
        resCdn.status_code,
        resCatbox.status_code,
        resCatbox.error.message );

    const auto catboxUrl = TrimWhitespace( resCatbox.text );
    qwr::QwrException::ExpectTrue(
        catboxUrl.starts_with( "https://" ),
        "Catbox returned unexpected response: {}",
        catboxUrl );

    return catboxUrl;
}

} // namespace

namespace drp
{

std::optional<qwr::u8string> UploadArt( const metadb_handle_ptr& handle, const qwr::u8string& uploadCommand )
{
    auto& aborter = qwr::GlobalAbortCallback::GetInstance();

    const auto artDataOpt = GetArtData( handle, aborter );
    if ( !artDataOpt || !artDataOpt->pArtData.is_valid() || artDataOpt->pArtData->get_size() == 0 )
    {
        return std::nullopt;
    }
    const auto& artData = *artDataOpt;

    aborter.check();

    const auto tStart = std::chrono::steady_clock::now();
    auto uploadBytes = ResizeCoverToJpegInMemory( artData.pArtData->get_ptr(), artData.pArtData->get_size(), kMaxCoverDimension );
    if ( uploadBytes.empty() )
    {
        const auto* rawPtr = static_cast<const uint8_t*>( artData.pArtData->get_ptr() );
        uploadBytes.assign( rawPtr, rawPtr + artData.pArtData->get_size() );
    }

    aborter.check();

    if ( IsBuiltinCatboxCommand( uploadCommand ) )
    {
        const auto artUrl = UploadWithFallback( uploadBytes, aborter );
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - tStart ).count();
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": uploaded " << uploadBytes.size() << "B in " << elapsedMs << "ms -> " << artUrl;
        return artUrl;
    }

    const auto artPath = SaveBufferToTempJpeg( uploadBytes, aborter );
    aborter.check();

    if ( config::advanced::logUploaderCmds )
    {
        LogDebug( "Upload command: `{} {}`", uploadCommand, artPath );
    }
    const auto artUrl = [&] {
        SubprocessExecutor uploader{ uploadCommand };

        uploader.Start();
        uploader.WriteData( artPath );
        const auto exitCode = uploader.WaitUntilCompleted( kMaxWaitTime );
        const auto outputOpt = uploader.GetOutput();
        const auto errorOpt = uploader.GetErrorOutput();
        qwr::QwrException::ExpectTrue(
            !exitCode,
            "Uploader failed with error code {}\n"
            "  stdout:\n"
            "{}\n"
            "  stderr:\n"
            "{}\n",
            exitCode,
            outputOpt.value_or( "" ),
            errorOpt.value_or( "" ) );
        qwr::QwrException::ExpectTrue( outputOpt.has_value(), "Uploader process didn't write anything to stdout" );

        return *outputOpt;
    }();
    if ( config::advanced::logUploaderOutput )
    {
        LogDebug( "Uploaded url: `{}`", artUrl );
    }

    return artUrl;
}

} // namespace drp  