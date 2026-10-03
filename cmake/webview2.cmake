# Microsoft Edge WebView2 SDK (BSD-3-Clause, see the package's LICENSE.txt): the headers and the static loader, so
# Melange.exe ships without WebView2Loader.dll. Pinned by version and SHA-256 of the .nupkg (a zip). For offline
# builds point FETCHCONTENT_SOURCE_DIR_WEBVIEW2 at an extracted copy.
set(WEBVIEW2_VERSION 1.0.4258.31)
FetchContent_Declare(webview2
    URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WEBVIEW2_VERSION}
    URL_HASH SHA256=56f7f4b8bf9aee4b8efefbbdd4f67d5f74ebd1b100ed0806da71bf76af481aa9
    DOWNLOAD_NAME webview2-${WEBVIEW2_VERSION}.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(webview2)
add_library(webview2_loader STATIC IMPORTED)
set_target_properties(webview2_loader PROPERTIES
    IMPORTED_LOCATION ${webview2_SOURCE_DIR}/build/native/x86/WebView2LoaderStatic.lib
    INTERFACE_INCLUDE_DIRECTORIES ${webview2_SOURCE_DIR}/build/native/include)
