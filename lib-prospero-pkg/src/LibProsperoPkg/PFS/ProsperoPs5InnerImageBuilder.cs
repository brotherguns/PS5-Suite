// LibProsperoPkg - A library for building and inspecting PS5 packages.
// Copyright (C) 2026 SvenGDK
//
// ---------------------------------------------------------------------------------------------------
// PS5 nwonly INNER pfs_image.dat assembler. Lays out the inner files data-first (raw files block-aligned,
// compressed files packed), followed by the block-info table and the Kraken-compressed metadata block.
// ---------------------------------------------------------------------------------------------------
#nullable enable
using LibProsperoPkg.PFS.Compression;
using System;
using System.Collections.Generic;
using System.IO;

namespace LibProsperoPkg.PFS;

/// <summary>One inner-image payload (a file's data or the metadata block) with its resolved on-disk placement.</summary>
public sealed class ProsperoPs5InnerPayload
{
    /// <summary>
    /// The payload bytes. Mutually exclusive with <see cref="SourcePath"/>; when a source file is set the
    /// payload is read from disk instead, so large payloads never occupy managed memory.
    /// </summary>
    public byte[]? Data;

    /// <summary>File supplying the payload bytes (a raw source file or a staged compressed-payload fragment).</summary>
    public string? SourcePath;

    /// <summary>Byte offset inside <see cref="SourcePath"/> where the payload begins.</summary>
    public long SourceOffset;

    /// <summary>Payload length in bytes. Defaults to <see cref="Data"/> length, or the remainder of
    /// <see cref="SourcePath"/> from <see cref="SourceOffset"/> when negative.</summary>
    public long SourceLength = -1;

    /// <summary>When true the payload is stored raw (never compressed) and is placed block-aligned.</summary>
    public bool StoreRaw;

    /// <summary>When true the payload is placed at the next 64 KiB block boundary; otherwise packed contiguously.</summary>
    public bool BlockAligned;

    /// <summary>When true the on-disk cursor is advanced to the next 64 KiB block boundary <em>after</em> this
    /// payload, so it occupies whole blocks and the following payload starts block-aligned. Used for the
    /// sce_sys subtree, which forms a fully block-aligned region.</summary>
    public bool BlockAlignedAfter;

    /// <summary>The payload's byte length, from <see cref="Data"/> or the source-file slice.</summary>
    public long PayloadLength
    {
        get
        {
            if (Data is { } d) return d.LongLength;
            if (SourcePath is { } p)
                return SourceLength >= 0 ? SourceLength : new FileInfo(p).Length - SourceOffset;
            return 0;
        }
    }

    /// <summary>Materializes the payload into a byte array (memory path only).</summary>
    public byte[] ReadAll()
    {
        if (Data is { } d) return d;
        if (SourcePath is null) return Array.Empty<byte>();
        long len = PayloadLength;
        if (len > Array.MaxLength)
            throw new InvalidDataException(
                $"Payload of {len:N0} bytes cannot be materialized into a single array; use the streaming build.");
        var bytes = new byte[(int)len];
        using var src = new FileStream(SourcePath, FileMode.Open, FileAccess.Read, FileShare.Read);
        src.Position = SourceOffset;
        src.ReadExactly(bytes);
        return bytes;
    }

    /// <summary>Writes the payload content to <paramref name="destination"/> at its current position.</summary>
    public void WriteContentTo(Stream destination)
    {
        if (Data is { } d)
        {
            destination.Write(d, 0, d.Length);
            return;
        }
        if (SourcePath is null) return;
        using var src = new FileStream(SourcePath, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
        src.Position = SourceOffset;
        long remaining = PayloadLength;
        var buf = new byte[1 << 20];
        while (remaining > 0)
        {
            int n = src.Read(buf, 0, (int)Math.Min(buf.Length, remaining));
            if (n <= 0) break;
            destination.Write(buf, 0, n);
            remaining -= n;
        }
    }
}

/// <summary>
/// Assembles the inner <c>pfs_image.dat</c>: data-first per-file layout (raw files block-aligned,
/// compressed files packed), a 32-byte block-info table, then the compressed metadata.
/// </summary>
public sealed class ProsperoPs5InnerImageBuilder
{
    /// <summary>The inner-image block size (64 KiB).</summary>
    public const int BlockSize = 0x10000;

    /// <summary>The per-file Kraken compression block size (256 KiB).</summary>
    public const int CompressBlockSize = 0x40000;

    private static int AlignUp(int v, int a) => (v + a - 1) & ~(a - 1);
    private static long AlignUp(long v, long a) => (v + a - 1) & ~(a - 1);

    /// <summary>
    /// Compresses a payload into its concatenated on-disk bytes (256 KiB blocks), or returns the caller's
    /// own array unchanged when <paramref name="storeRaw"/> is set.
    /// </summary>
    public static byte[] CompressPayload(byte[] raw, bool storeRaw)
        => CompressPayload(raw, storeRaw, out _);

    /// <summary>
    /// As <see cref="CompressPayload(byte[], bool)"/>, but also returns the parsed
    /// <see cref="ProsperoCompressedPfsFile"/> (its per-block chunk table) when the payload is stored
    /// compressed, so callers that need the block boundaries (e.g. the naps generator) do not have to
    /// Kraken-pack the same buffer a second time. <paramref name="compressedFile"/> is <see langword="null"/>
    /// when the payload is stored raw (either <paramref name="storeRaw"/> or the 6.25% keep rule fell back).
    /// </summary>
    public static byte[] CompressPayload(byte[] raw, bool storeRaw, out ProsperoCompressedPfsFile? compressedFile)
    {
        compressedFile = null;
        if (storeRaw) return raw;
        var pf = ProsperoCompressedPfsFile.Parse(ProsperoCompressedPfsImage.Pack(raw, 7, CompressBlockSize));
        using var ms = new MemoryStream();
        foreach (var b in pf.Blocks)
        {
            var d = b.CompressedData.ToArray();
            ms.Write(d, 0, d.Length);
        }
        byte[] comp = ms.ToArray();
        // Each block has already made its own store decision, so the compressed form never exceeds the
        // raw form and there is no file-level threshold on top of it.
        compressedFile = pf;
        return comp;
    }

    /// <summary>
    /// Assembles the inner image. <paramref name="payloads"/> are, in on-disk order, the data files followed by
    /// the block-info table payload and the metadata block. Each payload is compressed per its flags and placed
    /// block-aligned or packed. Returns the block-aligned-tail on-disk image.
    /// </summary>
    public byte[] Build(IReadOnlyList<ProsperoPs5InnerPayload> payloads)
    {
        // First pass: compress + resolve offsets.
        var chunks = new List<(long offset, byte[] data)>();
        long pos = 0;
        foreach (var p in payloads)
        {
            byte[] data = CompressPayload(p.ReadAll(), p.StoreRaw);
            if (p.BlockAligned)
                pos = AlignUp(pos, BlockSize);
            chunks.Add((pos, data));
            pos += data.Length;
            if (p.BlockAlignedAfter)
                pos = AlignUp(pos, BlockSize);
        }

        byte[] img = new byte[checked((int)pos)];
        foreach (var (offset, data) in chunks)
            Array.Copy(data, 0, img, (int)offset, data.Length);
        return img;
    }

    /// <summary>
    /// Streams the assembled inner image into <paramref name="destination"/> (seekable), writing every
    /// payload at its resolved offset without materializing the whole image in memory. Returns the
    /// block-aligned-tail on-disk image length. File-sourced payloads must be pre-compressed
    /// (<see cref="ProsperoPs5InnerPayload.StoreRaw"/>), which is exactly what the assembler produces.
    /// </summary>
    /// <param name="destination">A seekable, writable stream; the image is written from position 0.</param>
    /// <param name="payloads">Data files followed by the block-info table payload and the metadata block.</param>
    public long BuildToStream(Stream destination, IReadOnlyList<ProsperoPs5InnerPayload> payloads)
    {
        ArgumentNullException.ThrowIfNull(destination);
        if (!destination.CanSeek || !destination.CanWrite)
            throw new ArgumentException("Destination must be seekable and writable.", nameof(destination));

        long pos = 0;
        destination.Position = 0;
        foreach (var p in payloads)
        {
            if (!p.StoreRaw && p.SourcePath is not null)
                throw new InvalidOperationException(
                    "File-sourced inner payloads must be pre-compressed (StoreRaw); compression is the assembler's pass.");
            if (p.BlockAligned)
                pos = AlignUp(pos, BlockSize);
            if (destination.Position != pos)
                destination.Position = pos;
            p.WriteContentTo(destination);
            pos += p.PayloadLength;
            if (p.BlockAlignedAfter)
                pos = AlignUp(pos, BlockSize);
        }
        destination.SetLength(pos);
        destination.Flush();
        return pos;
    }
}
