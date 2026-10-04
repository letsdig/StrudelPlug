/*
  ==============================================================================

    StrudelFetch.h
    Downloads the latest @strudel/repl build into the user's app-data dir on
    first use. Strudel is AGPL and is not redistributed with the plugin.

    The package tarball is pulled straight from the npm registry and unpacked
    locally (gzip via JUCE, tar parsed here), so every file under dist/ lands
    on disk without having to guess hashed chunk names from index.js.

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>
#include <functional>

namespace StrudelFetch
{
    static const juce::String registryUrl = "https://registry.npmjs.org/@strudel/repl/latest";
    static const juce::String tarPrefix   = "package/dist/";   // only this subtree is kept

    inline juce::File directory()
    {
        auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
       #if JUCE_MAC
        base = base.getChildFile ("Application Support");
       #endif
        return base.getChildFile ("StrudelPlug").getChildFile ("strudel");
    }

    // Cached forever once present; delete the directory to force an update.
    inline bool isInstalled()
    {
        return directory().getChildFile ("index.js").existsAsFile();
    }

    inline std::unique_ptr<juce::InputStream> open (const juce::String& url, juce::String& error)
    {
        auto stream = juce::URL (url).createInputStream (
            juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress).withConnectionTimeoutMs (15000));
        if (stream == nullptr)
            error = "download failed: " + url;
        return stream;
    }

    // Resolves the "latest" dist-tag to its tarball URL via the registry metadata.
    inline bool resolveTarball (juce::String& tarballUrl, juce::String& error)
    {
        auto stream = open (registryUrl, error);
        if (stream == nullptr)
            return false;

        auto json = juce::JSON::parse (stream->readEntireStreamAsString());
        tarballUrl = json.getProperty ("dist", {}).getProperty ("tarball", {}).toString();
        if (tarballUrl.isEmpty())
        {
            error = "registry metadata missing dist.tarball";
            return false;
        }
        return true;
    }

    inline juce::String tarString (const char* field, int maxLen)
    {
        int len = 0;
        while (len < maxLen && field[len] != 0) ++len;
        return juce::String (juce::CharPointer_UTF8 (field), (size_t) len);
    }

    // Minimal ustar/PAX reader: writes regular files under tarPrefix into dest, skips the rest.
    inline bool extractTar (juce::InputStream& tar, const juce::File& dest, juce::String& error)
    {
        char header[512];
        juce::String longName;   // pending name from a GNU 'L' or PAX 'x' record

        for (;;)
        {
            if (tar.read (header, 512) != 512 || header[0] == 0)
                return true;     // end-of-archive marker (or plain EOF)

            juce::int64 entrySize = 0;   // octal
            for (int i = 124; i < 136 && header[i] >= '0' && header[i] <= '7'; ++i)
                entrySize = entrySize * 8 + (header[i] - '0');

            auto name = tarString (header, 100);
            if (tarString (header + 257, 5) == "ustar")
            {
                auto prefix = tarString (header + 345, 155);
                if (prefix.isNotEmpty())
                    name = prefix + "/" + name;
            }
            if (longName.isNotEmpty())
            {
                name = longName;
                longName.clear();
            }

            const char type = header[156];
            const auto padded = (entrySize + 511) & ~(juce::int64) 511;

            if (type == 'L' || type == 'x')
            {
                juce::MemoryBlock meta;
                tar.readIntoMemoryBlock (meta, (ssize_t) entrySize);
                const auto text = tarString ((const char*) meta.getData(), (int) meta.getSize());
                if (type == 'L')
                    longName = text;
                else
                    for (auto line : juce::StringArray::fromLines (text))
                        if (line.contains (" path="))
                            longName = line.fromFirstOccurrenceOf (" path=", false, false);
                tar.skipNextBytes (padded - entrySize);
                continue;
            }

            if ((type == '0' || type == 0) && name.startsWith (tarPrefix))
            {
                juce::MemoryBlock data;
                if ((juce::int64) tar.readIntoMemoryBlock (data, (ssize_t) entrySize) != entrySize)
                {
                    error = "truncated tarball at " + name;
                    return false;
                }
                auto file = dest.getChildFile (name.substring (tarPrefix.length()));
                file.getParentDirectory().createDirectory();
                if (! file.replaceWithData (data.getData(), data.getSize()))
                {
                    error = "cannot write " + file.getFullPathName();
                    return false;
                }
                tar.skipNextBytes (padded - entrySize);
            }
            else
            {
                tar.skipNextBytes (padded);
            }
        }
    }

    // Blocking; run off the message thread.
    inline bool download (juce::String& error)
    {
        juce::String tarballUrl;
        if (! resolveTarball (tarballUrl, error))
            return false;

        auto stream = open (tarballUrl, error);
        if (stream == nullptr)
            return false;

        // Unpack into a staging dir, then swap in, so isInstalled() implies complete.
        auto target  = directory();
        auto staging = target.getSiblingFile ("strudel.partial");
        staging.deleteRecursively();

        juce::GZIPDecompressorInputStream gunzip (stream.get(), false, juce::GZIPDecompressorInputStream::gzipFormat);
        const bool ok = extractTar (gunzip, staging, error);

        if (! ok || ! staging.getChildFile ("index.js").existsAsFile())
        {
            if (ok) error = "tarball has no dist/index.js";
            staging.deleteRecursively();
            return false;
        }

        target.deleteRecursively();
        target.getParentDirectory().createDirectory();
        if (! staging.moveFileTo (target))
        {
            error = "cannot move " + staging.getFullPathName();
            return false;
        }
        return true;
    }

    // Non-blocking; `done` is called on the message thread.
    inline void downloadAsync (std::function<void (bool ok, juce::String error)> done)
    {
        juce::Thread::launch ([done]
        {
            juce::String error;
            const bool ok = download (error);
            juce::MessageManager::callAsync ([done, ok, error] { done (ok, error); });
        });
    }
}
