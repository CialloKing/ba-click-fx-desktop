#include "appx_signer.hpp"
#include "identity_signer_options.hpp"

#include <iostream>
#include <string_view>
#include <vector>

namespace
{

void printUsage()
{
    std::cerr
        << "Usage: BAFX.IdentitySigner.exe --package <absolute.msix> "
        << "--thumbprint <40-hex-sha1> --store-location LocalMachine\n";
}

}

int wmain(const int argumentCount, wchar_t** arguments)
{
    try
    {
        std::vector<std::wstring_view> argumentViews;
        if (argumentCount > 1)
        {
            argumentViews.reserve(static_cast<std::size_t>(argumentCount - 1));
        }
        for (int index = 1; index < argumentCount; ++index)
        {
            argumentViews.emplace_back(arguments[index]);
        }

        const bafx::identity_signer::Options options =
            bafx::identity_signer::parseOptions(argumentViews);
        bafx::identity_signer::signPackage(options);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "BAFX.IdentitySigner: " << error.what() << '\n';
        printUsage();
        return 1;
    }
}
