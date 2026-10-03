// LibProsperoPkg - A library for building and inspecting PS5 packages.
// Copyright (C) 2026 SvenGDK
//
// ---------------------------------------------------------------------------------------------------
// PS5 nwonly INNER pfs_image.dat READER (the inverse of ProsperoPs5InnerImageAssembler). A data-first
// inner image has NO on-disk block table: it is a raw concatenation of per-file payloads (raw or
// header-stripped Kraken chunks) followed by the Kraken-compressed metadata region. The per-block
// geometry (on-disk offset, compressed/uncompressed sizes, raw-vs-Kraken flag, even/odd split, byte
// shuffle) lives ONLY in the sibling naps_pkg_layout.dat's CblockInfo section.
//
// This reconstructs the uncompressed PFS mount from (inner image bytes + naps bytes) so the standard
// ProsperoPfsReader can walk it. The naps CblockInfo section supplies the per-block decode geometry for
// all file data and the metadata region.
// ---------------------------------------------------------------------------------------------------
#nullable enable
using LibProsperoPkg.PFS.Compression.Oodle;
using LibProsperoPkg.PKG;
using LibProsperoPkg.Util;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace LibProsperoPkg.PFS;

/// <summary>A regular file discovered in a reconstructed inner mount.</summary>
public sealed class ProsperoPs5InnerFileEntry
{
    /// <summary>User-root-relative path (e.g. <c>sce_sys/keystone</c> or <c>application.ps.bundle</c>).</summary>
    public required string Path { get; init; }

    /// <summary>Byte offset of the file's (already decoded) data within the reconstructed mount.</summary>
    public required ulong LogicalOffset { get; init; }

    /// <summary>File size in bytes.</summary>
    public required long Size { get; init; }
}

/// <summary>The reconstructed inner PFS mount plus the offset of its superblock.</summary>
public sealed class ProsperoPs5InnerMountResult
{
    /// <summary>The uncompressed inner PFS mount bytes (walk with <see cref="ProsperoPfsReader"/>).</summary>
    public required byte[] Mount { get; init; }

    /// <summary>
    /// Byte offset of the PFS superblock within <see cref="Mount"/>. A nwonly inner image is data-first,
    /// so the superblock is at the metadata-region base (not offset 0).
    /// </summary>
    public required long SuperblockOffset { get; init; }
}

/// <summary>
/// Reconstructs the uncompressed inner PFS mount of a PS5 nwonly package from its data-first
/// <c>pfs_image.dat</c> and the sibling <c>naps_pkg_layout.dat</c>. The result is a plaintext PFS
/// image whose superblock is at the metadata-region base that <see cref="ProsperoPfsReader"/> can walk.
/// </summary>
public static class ProsperoPs5InnerImageReader
{
    private const int Chunk128K = 0x20000;   // even/odd sub-chunk split
    private const long Ublock256K = 0x40000; // one CblockInfo STD covers up to one 256K ublock

    /// <summary>
    /// Reconstructs the uncompressed PFS mount from a data-first inner image and its naps layout. Files
    /// and the metadata region are decoded to their logical offsets; unreferenced inter-file padding is
    /// left zero-filled.
    /// </summary>
    /// <param name="innerImage">The on-disk data-first <c>pfs_image.dat</c> bytes.</param>
    /// <param name="naps">The <c>naps_pkg_layout.dat</c> bytes (its CblockInfo drives the decode).</param>
    /// <param name="mountSize">
    /// The uncompressed mount size (Ndblock·64K). Pass 0 to derive it from the naps: the mount end is the
    /// largest 64K-aligned <c>fidx</c> boundary (trailing typed sentinels are not block-aligned).
    /// </param>
    /// <param name="flagHints">Optional per-CblockInfo Kraken flag bytes; see
    /// <see cref="ReconstructMountToStream"/>.</param>
    public static ProsperoPs5InnerMountResult ReconstructMount(
        ReadOnlySpan<byte> innerImage, ReadOnlySpan<byte> naps, long mountSize = 0,
        IReadOnlyList<int>? flagHints = null)
    {
        NapsLayoutDocument doc = ProsperoNapsLayout.Parse(naps);
        IReadOnlyList<NapsCblockInfoEntry> cb = doc.CblockInfos;

        long[] rawOffsets = doc.FileOffsets.Select(f => (long)f.UncompressedOffsetStart).ToArray();
        if (mountSize <= 0)
        {
            // Derive the mount size: the mount end is a 64K-aligned fidx boundary; typed/garbage sentinels
            // are not block-aligned, so the largest block-aligned boundary is Ndblock*64K.
            mountSize = rawOffsets.Where(v => v > 0 && (v & 0xFFFF) == 0).DefaultIfEmpty(0).Max();
        }
        if (mountSize <= 0 || mountSize > int.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(mountSize), mountSize, "Implausible inner mount size.");

        // fidx = uncompressed file-offset boundaries. Only values inside the mount are real block
        // boundaries; trailing sentinels (huge/typed) are ignored by the min-above-cursor lookup.
        long[] boundaries = rawOffsets
            .Where(v => v > 0 && v <= mountSize)
            .Distinct().OrderBy(v => v).ToArray();

        // The metadata region base (= superblock offset) is the last file-offset boundary below the mount
        // end; the data files occupy everything below it and the metadata PFS is stored above it.
        long metaBase = boundaries.LastOrDefault(v => v < mountSize);

        var mount = new byte[mountSize];
        byte[] innerArr = innerImage.ToArray();

        long onDisk = 0, uncompOff = 0;
        for (int i = 0; i < cb.Count; i++)
        {
            NapsCblockInfoEntry e = cb[i];
            if (e.IsRunBase)
            {
                // On-disk cursor re-anchors at each run: 32K-granular base + the fractional byte offset
                // carried in the following STD's low-15 coffMod bits.
                long frac = (i + 1 < cb.Count && !cb[i + 1].IsRunBase)
                    ? (cb[i + 1].CoffsetStartMod256K & 0x7fff) : 0;
                onDisk = ((long)e.TweakIdxStart << 15) + frac;
                continue;
            }

            long fileEnd = NextBoundary(boundaries, uncompOff, mountSize);
            long uncompLen = Math.Min(Ublock256K, fileEnd - uncompOff);
            if (uncompLen <= 0 || i + 1 >= cb.Count)
                break;

            int evenComp = (int)(e.ClenEvenMinus1 + 1);
            NapsCblockInfoEntry next = cb[i + 1];
            long thisRel = e.CoffsetStartMod256K;
            long nextRel = next.IsRunBase ? next.CoffsetEndMod256K : next.CoffsetStartMod256K;
            // The compressed-offset fields are mod-0x40000; mask the delta so a block whose compressed
            // span wraps the 256 KiB window still yields its true length.
            int totalComp = (int)((nextRel - thisRel) & 0x3FFFF);
            bool kraken = e.KdePredictor == 2;
            // Builder extension: a nonzero STD byte-8 carries the true Kraken flag byte; validated
            // foreign dumps leave it zero, falling back to the caller hints / candidate guessing.
            int? flagHint = e.DecodeFlagHint != 0 ? e.DecodeFlagHint
                : flagHints is not null && i < flagHints.Count && flagHints[i] >= 0 ? flagHints[i] : null;

            DecodeBlockInto(innerArr, onDisk, totalComp, evenComp, (int)uncompLen, kraken,
                            mount, (int)uncompOff, flagHint);

            uncompOff += uncompLen;
            onDisk += kraken ? totalComp : uncompLen;
        }

        // Locate the metadata superblock (version 2 + magic 20130315) at a 64K boundary. The metadata PFS
        // sits at the top of the mount, so scan downward and take the highest match; fall back to the
        // fidx-derived base. This is robust to per-package fidx layout differences.
        long superblockOffset = metaBase;
        for (long p = (mountSize - 0x10000) & ~0xFFFFL; p >= 0; p -= 0x10000)
        {
            if (System.Buffers.Binary.BinaryPrimitives.ReadInt64LittleEndian(mount.AsSpan((int)p)) == 2 &&
                System.Buffers.Binary.BinaryPrimitives.ReadInt64LittleEndian(mount.AsSpan((int)p + 8)) == 20130315)
            {
                superblockOffset = p;
                break;
            }
        }

        return new ProsperoPs5InnerMountResult { Mount = mount, SuperblockOffset = superblockOffset };
    }

    /// <summary>
    /// Streaming equivalent of <see cref="ReconstructMount"/>: decodes the data-first inner image block
    /// by block and writes the reconstructed mount sequentially into <paramref name="destination"/>.
    /// Neither the image nor the mount is held in memory, so mounts larger than the 2 GiB array limit
    /// (and any mount the format allows) can be reconstructed. Returns the metadata superblock offset.
    /// </summary>
    /// <param name="innerImage">Random-access view over the on-disk <c>pfs_image.dat</c> bytes.</param>
    /// <param name="naps">The <c>naps_pkg_layout.dat</c> bytes (its CblockInfo drives the decode).</param>
    /// <param name="destination">Seekable, writable stream; receives the mount bytes from offset 0.</param>
    /// <param name="mountSize">The uncompressed mount size (Ndblock·64K), or 0 to derive it.</param>
    /// <param name="flagHints">
    /// Optional per-CblockInfo Kraken flag bytes (index-aligned with the parsed CblockInfo list; -1 for
    /// non-applicable entries). The 9-byte STD record carries no sub-chunk type/literal-model field, so
    /// without hints each block is decoded by trying the observed flag combinations — a heuristic that can
    /// accept a wrong-but-clean-decoding combination. Builders verifying their own output pass the true
    /// flags (<see cref="NapsLayoutDocument.StdDecodeFlags"/>); extraction of foreign images passes null.
    /// </param>
    public static long ReconstructMountToStream(
        IMemoryReader innerImage, ReadOnlySpan<byte> naps, Stream destination, long mountSize = 0,
        IReadOnlyList<int>? flagHints = null)
    {
        ArgumentNullException.ThrowIfNull(innerImage);
        ArgumentNullException.ThrowIfNull(destination);
        if (!destination.CanSeek || !destination.CanWrite)
            throw new ArgumentException("Destination must be seekable and writable.", nameof(destination));

        NapsLayoutDocument doc = ProsperoNapsLayout.Parse(naps);
        IReadOnlyList<NapsCblockInfoEntry> cb = doc.CblockInfos;

        long[] rawOffsets = doc.FileOffsets.Select(f => (long)f.UncompressedOffsetStart).ToArray();
        if (mountSize <= 0)
            mountSize = rawOffsets.Where(v => v > 0 && (v & 0xFFFF) == 0).DefaultIfEmpty(0).Max();
        if (mountSize <= 0)
            throw new ArgumentOutOfRangeException(nameof(mountSize), mountSize, "Implausible inner mount size.");

        long[] boundaries = rawOffsets
            .Where(v => v > 0 && v <= mountSize)
            .Distinct().OrderBy(v => v).ToArray();

        long metaBase = boundaries.LastOrDefault(v => v < mountSize);

        destination.SetLength(mountSize);
        destination.Position = 0;

        long onDisk = 0, uncompOff = 0;
        for (int i = 0; i < cb.Count; i++)
        {
            NapsCblockInfoEntry e = cb[i];
            if (e.IsRunBase)
            {
                long frac = (i + 1 < cb.Count && !cb[i + 1].IsRunBase)
                    ? (cb[i + 1].CoffsetStartMod256K & 0x7fff) : 0;
                onDisk = ((long)e.TweakIdxStart << 15) + frac;
                continue;
            }

            long fileEnd = NextBoundary(boundaries, uncompOff, mountSize);
            long uncompLen = Math.Min(Ublock256K, fileEnd - uncompOff);
            if (uncompLen <= 0 || i + 1 >= cb.Count)
                break;

            int evenComp = (int)(e.ClenEvenMinus1 + 1);
            NapsCblockInfoEntry next = cb[i + 1];
            long thisRel = e.CoffsetStartMod256K;
            long nextRel = next.IsRunBase ? next.CoffsetEndMod256K : next.CoffsetStartMod256K;
            // The compressed-offset fields are mod-0x40000; mask the delta so a block whose compressed
            // span wraps the 256 KiB window still yields its true length.
            int totalComp = (int)((nextRel - thisRel) & 0x3FFFF);
            bool kraken = e.KdePredictor == 2;
            // Builder extension: a nonzero STD byte-8 carries the true Kraken flag byte; validated
            // foreign dumps leave it zero, falling back to the caller hints / candidate guessing.
            int? flagHint = e.DecodeFlagHint != 0 ? e.DecodeFlagHint
                : flagHints is not null && i < flagHints.Count && flagHints[i] >= 0 ? flagHints[i] : null;

            string? issue = DecodeBlockToStream(innerImage, onDisk, totalComp, evenComp, (int)uncompLen, kraken,
                                destination, uncompOff, flagHint);
            if (issue != null)
                DecodeDiag?.Invoke($"cb[{i}] uncompOff=0x{uncompOff:X} onDisk=0x{onDisk:X} " +
                    $"totalComp=0x{totalComp:X} evenComp={evenComp} ulen=0x{uncompLen:X} kraken={kraken} " +
                    $"hint={(flagHint?.ToString("X2") ?? "-")} thisRel=0x{thisRel:X} nextRel=0x{nextRel:X} " +
                    $"pred={e.KdePredictor} -> {issue}");

            uncompOff += uncompLen;
            onDisk += kraken ? totalComp : uncompLen;
        }

        // Locate the metadata superblock (version 2 + magic 20130315) at a 64K boundary, scanning down
        // from the mount end; fall back to the fidx-derived base.
        var probe = new byte[16];
        long superblockOffset = metaBase;
        for (long p = (mountSize - 0x10000) & ~0xFFFFL; p >= 0; p -= 0x10000)
        {
            destination.Position = p;
            if (destination.Read(probe, 0, 16) != 16) break;
            if (BitConverter.ToInt64(probe, 0) == 2 && BitConverter.ToInt64(probe, 8) == 20130315)
            {
                superblockOffset = p;
                break;
            }
        }

        return superblockOffset;
    }

    /// <summary>
    /// As <see cref="ReadFileTree(byte[], long)"/>, but reads the reconstructed mount through a
    /// random-access <see cref="IMemoryReader"/> (e.g. a <c>StreamReader</c> over a spooled mount file)
    /// so the mount is never materialized in memory.
    /// </summary>
    public static IReadOnlyList<ProsperoPs5InnerFileEntry> ReadFileTree(IMemoryReader mount, long metaBase)
    {
        var sb = new byte[0x40];
        mount.Read(metaBase, sb, 0, sb.Length);
        if (BitConverter.ToInt64(sb, 0) != 2 || BitConverter.ToInt64(sb, 8) != 20130315)
            throw new InvalidOperationException("Inner mount metadata does not start with a PS5 PFS superblock.");

        int blockSize = BitConverter.ToInt32(sb, 0x20);
        long inodeCount = BitConverter.ToInt64(sb, 0x30);
        if (blockSize <= 0 || inodeCount <= 0 || inodeCount > 1_000_000)
            throw new InvalidOperationException("Implausible inner metadata superblock (block size / inode count).");

        long inodeTable = metaBase + blockSize;
        var nodes = new InnerInode[inodeCount];
        var entry = new byte[0xA8];
        for (long i = 0; i < inodeCount; i++)
        {
            mount.Read(inodeTable + i * 0xA8, entry, 0, entry.Length);
            nodes[i] = new InnerInode
            {
                Mode = BitConverter.ToUInt16(entry, 0),
                Size = BitConverter.ToInt64(entry, 8),
                LogicalOffset = BitConverter.ToUInt64(entry, 0x60),
            };
        }

        var files = new List<ProsperoPs5InnerFileEntry>();
        WalkDir(mount, blockSize, nodes, 0, "", underUroot: false, files, new HashSet<uint>());
        return files;
    }

    private static void WalkDir(IMemoryReader mount, int blockSize, InnerInode[] nodes, uint dirInode,
        string path, bool underUroot, List<ProsperoPs5InnerFileEntry> files, HashSet<uint> seen)
    {
        if (dirInode >= nodes.Length || !seen.Add(dirInode))
            return;
        InnerInode dir = nodes[dirInode];
        if (!dir.IsDirectory || dir.Size <= 0)
            return;

        byte[] region;
        try { region = new byte[dir.Size]; }
        catch { return; }
        mount.Read((long)dir.LogicalOffset, region, 0, region.Length);
        using var ms = new MemoryStream(region, writable: false);
        while (ms.Position + 0x10 <= ms.Length)
        {
            long entryPos = ms.Position;
            ProsperoPfsDirent d;
            try { d = ProsperoPfsDirent.ReadFromStream(ms); }
            catch { break; }
            if (d.EntSize <= 0) break;
            ms.Position = entryPos + d.EntSize;

            string name = d.Name;
            if (string.IsNullOrEmpty(name) || name == "." || name == "..")
                continue;
            if (d.InodeNumber >= nodes.Length)
                continue;

            bool childUnderUroot = underUroot || name == "uroot";
            string childPath = path.Length == 0
                ? (name == "uroot" ? "" : name)
                : path + "/" + name;

            if (d.Type == ProsperoDirentType.Directory)
                WalkDir(mount, blockSize, nodes, d.InodeNumber, childPath, childUnderUroot, files, seen);
            else if (d.Type == ProsperoDirentType.File && underUroot && childPath.Length > 0)
                files.Add(new ProsperoPs5InnerFileEntry
                {
                    Path = childPath,
                    LogicalOffset = nodes[d.InodeNumber].LogicalOffset,
                    Size = nodes[d.InodeNumber].Size,
                });
        }
    }

    /// <summary>Diagnostic sink: invoked once per CblockInfo STD that fails to decode (test builds only).</summary>
    internal static event Action<string>? DecodeDiag;

    // Streaming counterpart of DecodeBlockInto: reads the on-disk payload through an IMemoryReader and
    // writes the decoded block at its uncompressed mount offset in the destination stream. Returns null
    // on success or a short reason string when the block was zero-filled instead.
    private static string? DecodeBlockToStream(IMemoryReader inner, long onDisk, int totalComp, int evenComp,
        int uncompLen, bool kraken, Stream mount, long mountOff, int? flagHint = null)
    {
        mount.Position = mountOff;

        if (!kraken)
        {
            // Raw store: copy through. The image may end inside the last block (stored size is not
            // padded to the logical length), so reads shrink on failure and the tail zero-fills.
            var chunk = new byte[Math.Min(uncompLen, 1 << 16)];
            int written = 0;
            while (written < uncompLen)
            {
                int want = Math.Min(chunk.Length, uncompLen - written);
                int got = 0;
                while (want > 0)
                {
                    try { inner.Read(onDisk + written, chunk, 0, want); got = want; break; }
                    catch { want >>= 1; }
                }
                if (got == 0) break;
                mount.Write(chunk, 0, got);
                written += got;
            }
            while (written < uncompLen)
            {
                int n = Math.Min(chunk.Length, uncompLen - written);
                Array.Clear(chunk, 0, n);
                mount.Write(chunk, 0, n);
                written += n;
            }
            return null;
        }

        if (onDisk < 0 || totalComp <= 0)
        {
            var z = new byte[uncompLen];
            mount.Write(z, 0, uncompLen);
            return "invalid geometry";
        }

        var src = new byte[totalComp];
        try { inner.Read(onDisk, src, 0, totalComp); }
        catch
        {
            var z = new byte[uncompLen];
            mount.Write(z, 0, uncompLen);
            return "read failed";
        }

        int firstChunkComp = uncompLen > Chunk128K ? evenComp : 0;
        var decoded = new byte[uncompLen];
        IEnumerable<int> candidates = flagHint is { } hint
            ? new[] { hint }.Concat(FlagCandidates(uncompLen > Chunk128K))
            : FlagCandidates(uncompLen > Chunk128K);
        foreach (int flags in candidates)
        {
            KrakenDecodeStatus st;
            try { st = KrakenDecoder.DecodeBlock(src, flags, firstChunkComp, decoded); }
            catch { continue; }
            if (st != KrakenDecodeStatus.Success)
                continue;
            mount.Write(decoded, 0, uncompLen);
            return null;
        }
        // Undecodable block: leave zeros (matches the in-memory path, which leaves the mount untouched).
        return "decode failed";
    }

    /// <summary>
    /// Parses the reconstructed inner mount's PS5 metadata (superblock + inode table + dirents) and returns
    /// every regular file under <c>uroot</c> with its user-root-relative path, logical offset and size. The
    /// file bytes are <c>mount[LogicalOffset .. LogicalOffset+Size]</c> (already decoded by
    /// <see cref="ReconstructMount"/>). The PS5 inner inode layout (LogicalOffset@0x60, parent@0x6c)
    /// differs from the layout <see cref="ProsperoPfsReader"/> reads, so this does not use it.
    /// </summary>
    public static IReadOnlyList<ProsperoPs5InnerFileEntry> ReadFileTree(byte[] mount, long metaBase)
    {
        ReadOnlySpan<byte> sb = mount.AsSpan((int)metaBase);
        if (System.Buffers.Binary.BinaryPrimitives.ReadInt64LittleEndian(sb) != 2 ||
            System.Buffers.Binary.BinaryPrimitives.ReadInt64LittleEndian(sb[8..]) != 20130315)
            throw new InvalidOperationException("Inner mount metadata does not start with a PS5 PFS superblock.");

        int blockSize = (int)System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(sb[0x20..]);
        int inodeCount = (int)System.Buffers.Binary.BinaryPrimitives.ReadInt64LittleEndian(sb[0x30..]);
        if (blockSize <= 0 || inodeCount <= 0 || inodeCount > 1_000_000)
            throw new InvalidOperationException("Implausible inner metadata superblock (block size / inode count).");

        // Parse the inode table (block 1). PS5 inner inode: Mode@0, Size@8, LogicalOffset@0x60, parent@0x6c.
        long inodeTable = metaBase + blockSize;
        var nodes = new InnerInode[inodeCount];
        for (int i = 0; i < inodeCount; i++)
        {
            ReadOnlySpan<byte> e = mount.AsSpan((int)(inodeTable + i * 0xA8), 0xA8);
            nodes[i] = new InnerInode
            {
                Mode = System.Buffers.Binary.BinaryPrimitives.ReadUInt16LittleEndian(e),
                Size = System.Buffers.Binary.BinaryPrimitives.ReadInt64LittleEndian(e[8..]),
                LogicalOffset = System.Buffers.Binary.BinaryPrimitives.ReadUInt64LittleEndian(e[0x60..]),
            };
        }

        // Walk the directory tree from the super-root (inode 0), collecting files under "uroot".
        var files = new List<ProsperoPs5InnerFileEntry>();
        WalkDir(mount, blockSize, nodes, 0, "", underUroot: false, files, new HashSet<uint>());
        return files;
    }

    private sealed class InnerInode
    {
        public ushort Mode;
        public long Size;
        public ulong LogicalOffset;
        public bool IsDirectory => (Mode & 0x4000) != 0;
    }

    private static void WalkDir(byte[] mount, int blockSize, InnerInode[] nodes, uint dirInode,
        string path, bool underUroot, List<ProsperoPs5InnerFileEntry> files, HashSet<uint> seen)
    {
        if (dirInode >= nodes.Length || !seen.Add(dirInode))
            return;
        InnerInode dir = nodes[dirInode];
        if (!dir.IsDirectory || dir.Size <= 0)
            return;

        long start = (long)dir.LogicalOffset;
        long end = Math.Min(start + dir.Size, mount.Length);
        using var ms = new System.IO.MemoryStream(mount, (int)start, (int)(end - start), writable: false);
        while (ms.Position + 0x10 <= ms.Length)
        {
            long entryPos = ms.Position;
            ProsperoPfsDirent d;
            try { d = ProsperoPfsDirent.ReadFromStream(ms); }
            catch { break; }
            if (d.EntSize <= 0) break;
            ms.Position = entryPos + d.EntSize;

            string name = d.Name;
            if (string.IsNullOrEmpty(name) || name == "." || name == "..")
                continue;
            if (d.InodeNumber >= nodes.Length)
                continue;

            // The super-root wraps the content under "uroot" (its other children are internal flat-path
            // tables); only files at or below uroot are real package content.
            bool childUnderUroot = underUroot || name == "uroot";
            string childPath = path.Length == 0
                ? (name == "uroot" ? "" : name)
                : path + "/" + name;

            if (d.Type == ProsperoDirentType.Directory)
                WalkDir(mount, blockSize, nodes, d.InodeNumber, childPath, childUnderUroot, files, seen);
            else if (d.Type == ProsperoDirentType.File && underUroot && childPath.Length > 0)
                files.Add(new ProsperoPs5InnerFileEntry
                {
                    Path = childPath,
                    LogicalOffset = nodes[d.InodeNumber].LogicalOffset,
                    Size = nodes[d.InodeNumber].Size,
                });
        }
    }

    private static long NextBoundary(long[] boundaries, long cur, long mountSize)
    {
        foreach (long b in boundaries)
            if (b > cur)
                return b;
        return mountSize;
    }

    // Decode a single block (files + metadata) into the mount at mountOff. Raw blocks copy uncompLen
    // bytes; Kraken blocks decode even (seeded) + odd (seedless, back-referencing even in the same
    // 256K buffer). Unreferenced padding blocks may copy garbage past their real footprint, which is
    // harmless because no inode points into the inter-file padding region.
    // NOTE: the naps records a per-block shuffle predictor, but the data-first payloads (data files and
    // the metadata region) are stored UN-shuffled on disk — decoding them yields the final plaintext
    // directly (applying the de-interleave would corrupt them). A package with genuinely shuffled blocks
    // would need a per-64K-sub-block de-interleave reintroduced here.
    private static void DecodeBlockInto(byte[] inner, long onDisk, int totalComp, int evenComp,
        int uncompLen, bool kraken, byte[] mount, int mountOff, int? flagHint = null)
    {
        int room = mount.Length - mountOff;
        if (room <= 0)
            return;
        int copyLen = Math.Min(uncompLen, room);

        if (!kraken)
        {
            // Raw store: copy the uncompressed bytes straight through (bounded by the image length).
            if (onDisk >= 0 && onDisk < inner.Length)
            {
                int avail = (int)Math.Min(copyLen, inner.Length - onDisk);
                Array.Copy(inner, onDisk, mount, mountOff, avail);
            }
            return;
        }

        if (onDisk < 0 || onDisk + totalComp > inner.Length || totalComp <= 0)
            return;

        var src = inner.AsSpan((int)onDisk, totalComp);
        int firstChunkComp = uncompLen > Chunk128K ? evenComp : 0;
        var decoded = new byte[uncompLen];

        // The per-chunk newLZ/literal-mode flag bits are not stored in the packed 9-byte CblockInfo, so
        // select them by trying the observed combinations and taking the first that decodes cleanly.
        // Order favours the common case (both sub-chunks newLZ + raw-literal, then sub/delta variants).
        // A supplied hint (builder-side verification) is tried first.
        IEnumerable<int> candidates = flagHint is { } hint
            ? new[] { hint }.Concat(FlagCandidates(uncompLen > Chunk128K))
            : FlagCandidates(uncompLen > Chunk128K);
        foreach (int flags in candidates)
        {
            KrakenDecodeStatus st;
            try { st = KrakenDecoder.DecodeBlock(src, flags, firstChunkComp, decoded); }
            catch { continue; }
            if (st != KrakenDecodeStatus.Success)
                continue;
            Array.Copy(decoded, 0, mount, mountOff, copyLen);
            return;
        }
    }

    private static int[] FlagCandidates(bool multiChunk) => multiChunk
        ? [0x22, 0x02, 0x12, 0x32, 0x23, 0x03, 0x13, 0x33, 0x00, 0x20]
        : [0x02, 0x00, 0x03, 0x01];
}
