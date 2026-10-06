// Usage:
//   test-stream verify <pkg> <srcFolder> <outDir> [passcode]
//                            -> validate + fully extract a real package and diff every file
//                               against the source tree (size + SHA-256).
//   test-stream diag <pkg> [passcode]
//                            -> manual diagnostic pipeline: FIH -> outer PFS decrypt ->
//                               naps/CblockInfo stats -> inner mount reconstruct -> file tree.
using System.Buffers.Binary;
using System.Security.Cryptography;
using LibProsperoPkg;
using LibProsperoPkg.PFS;
using LibProsperoPkg.PKG;
using LibProsperoPkg.Util;

if (args.Length >= 1 && args[0] == "verify")
    return await VerifyAsync(args[1], args[2], args[3], args.Length > 4 ? args[4] : new string('0', 32));
if (args.Length >= 1 && args[0] == "diag")
    return Diag(args[1], args.Length > 2 ? args[2] : new string('0', 32));
if (args.Length >= 1 && args[0] == "napsrt")
    return NapsRoundTrip();
if (args.Length >= 1 && args[0] == "pkgbuild")
    return await PkgBuildVerifyAsync(args.Length > 1 ? long.Parse(args[1]) : 48L << 20);
if (args.Length >= 1 && args[0] == "inodeoff")
    return InodeOffsetCheck();
if (args.Length >= 3 && args[0] == "realbuild")
    return RealBuild(args[1], args[2]);
if (args.Length >= 3 && args[0] == "homebrew")
    return HomebrewBuild(args[1], args[2]);
if (args.Length >= 3 && args[0] == "fself")
    return FselfWrap(args[1], args[2]);
if (args.Length >= 2 && args[0] == "cntdump")
    return CntDump(args[1]);

Console.WriteLine("usage: test-stream verify <pkg> <srcFolder> <outDir> [passcode]\n" +
                  "       test-stream diag <pkg> [passcode]\n" +
                  "       test-stream napsrt   (synthetic inner-image -> naps -> mount round-trip)\n" +
                  "       test-stream pkgbuild [srcBytes]  (full build -> extract -> diff round-trip)\n" +
                  "       test-stream inodeoff (inode LogicalOffset >4GiB serialization check)\n" +
                  "       test-stream homebrew <srcFolder> <outDir>  (license-free debug fPKG via ProsperoHomebrewPackager)");
return 0;

// ---------- napsrt: inner-image -> naps -> mount reconstruction round-trip ----------
static int NapsRoundTrip()
{
    string tmp = Path.Combine(Path.GetTempPath(), "napsrt-" + Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(tmp);
    try
    {
        // Synthetic payload set exercising every naps schedule shape:
        //  - a large compressible file (many 256K ublocks -> one STD each + mid-file RUN anchors)
        //  - a mixed file alternating compressible / incompressible 256K blocks (stored-in-compressed)
        //  - a small in-memory compressed file (<256K, Data-backed path)
        //  - a raw stored file
        var rng = new Random(12345);
        var sources = new Dictionary<string, string>(StringComparer.Ordinal); // inner path -> disk path
        var expectBytes = new Dictionary<string, byte[]>(StringComparer.Ordinal);

        string bigPath = Path.Combine(tmp, "big-src.bin");
        using (var fs = new FileStream(bigPath, FileMode.Create))
        {
            var tile = new byte[0x1000];
            for (int i = 0; i < tile.Length; i++) tile[i] = (byte)(i * 7 + (i >> 4));
            for (int i = 0; i < 64 * 1024 * 1024 / tile.Length; i++) fs.Write(tile);   // 64 MiB
        }
        sources["/big-compressible.dat"] = bigPath;

        string mixedPath = Path.Combine(tmp, "mixed-src.bin");
        using (var fs = new FileStream(mixedPath, FileMode.Create))
        {
            var comp = new byte[0x40000];
            for (int i = 0; i < comp.Length; i++) comp[i] = (byte)(i >> 6);           // compressible
            var rand = new byte[0x40000];
            rng.NextBytes(rand);                                                     // incompressible
            for (int i = 0; i < 16; i++) fs.Write(i % 2 == 0 ? rand : comp);         // 4 MiB mixed
        }
        sources["/mixed.dat"] = mixedPath;

        string rawPath = Path.Combine(tmp, "raw-src.bin");
        {
            var rand = new byte[0x20000];
            rng.NextBytes(rand);
            File.WriteAllBytes(rawPath, rand);
        }
        sources["/stored-raw.bin"] = rawPath;

        // Barely-compressible file: ~60% random bytes -> each 128 KiB sub-chunk compresses to
        // ~77 KiB, which EXCEEDS the 64 KiB cap of the buggy (v-1)*2 clenEvenMinus1 encoding.
        // This is the case the real 4 GB package hits ~11k times.
        string heavyPath = Path.Combine(tmp, "heavy-src.bin");
        {
            var tile = new byte[0x1000];
            for (int i = 0; i < tile.Length; i++) tile[i] = (byte)(i * 11 + (i >> 5));
            var heavy = new byte[4 * 0x40000]; // 4 ublocks
            for (long i = 0; i < heavy.LongLength; i++)
                heavy[i] = i % 5 < 3 ? (byte)rng.Next(256) : tile[i % tile.Length];
            File.WriteAllBytes(heavyPath, heavy);
        }
        sources["/heavy.dat"] = heavyPath;

        var smallData = new byte[0x30000];
        for (int i = 0; i < smallData.Length; i++) smallData[i] = (byte)(i * 3);
        expectBytes["/small-mem.dat"] = smallData;

        var files = new List<ProsperoPs5InnerFile>();
        foreach (var (inner, disk) in sources.OrderBy(kv => kv.Key))
        {
            files.Add(new ProsperoPs5InnerFile { Path = inner, SourcePath = disk });
            expectBytes[inner] = File.ReadAllBytes(disk);
        }
        files.Add(new ProsperoPs5InnerFile { Path = "/small-mem.dat", Data = smallData });

        Console.WriteLine("== Building inner image (file-backed) ==");
        var asm = new ProsperoPs5InnerImageAssembler(0x65000000, 0);
        var result = asm.Build(files, tmp);
        Console.WriteLine($"  image: {result.ImageLength:N0} bytes at {result.ImagePath}");
        Console.WriteLine($"  mountSize=0x{result.Ndblock * 0x10000:X} metaBase=0x{result.MetaBaseLogical:X} " +
                          $"dataEnd=0x{result.DataEndLogical:X} placements={result.Placements.Count}");
        foreach (var p in result.Placements)
            Console.WriteLine($"    onDisk=0x{p.OnDiskOffset:X} logical=0x{p.LogicalOffset:X} " +
                              $"diskSz={p.OnDiskSize:N0} uncomp={p.UncompressedSize:N0} raw={p.StoreRaw} blocks={p.Blocks?.Count ?? 0}");

        Console.WriteLine("== Generating naps_pkg_layout.dat ==");
        var genDoc = ProsperoNwonlyNapsGenerator.GenerateDocument(result);
        byte[] naps = ProsperoNapsLayout.BuildLayout(genDoc);
        Console.WriteLine($"  naps: {naps.Length:N0} bytes");
        var doc = ProsperoNapsLayout.Parse(naps);
        int kraken = doc.CblockInfos.Count(e => !e.IsRunBase && e.KdePredictor == 2);
        int runB = doc.CblockInfos.Count(e => e.IsRunBase);
        Console.WriteLine($"  parsed: fileOffsets={doc.FileOffsets.Count} cblockInfos={doc.CblockInfos.Count} " +
                          $"(kraken={kraken} raw={doc.CblockInfos.Count - kraken - runB} runBase={runB}) " +
                          $"u2c={doc.CblockInfoOffsetByUblock.Count}");
        for (int ci = 0; ci < Math.Min(12, doc.CblockInfos.Count); ci++)
        {
            var ce = doc.CblockInfos[ci];
            Console.WriteLine(ce.IsRunBase
                ? $"    cb[{ci}] RUN tweakStart={ce.TweakIdxStart} coffStart256K=0x{ce.CoffsetStart256K:X} coffEndMod=0x{ce.CoffsetEndMod256K:X} keyIdx={ce.KeyTableIdx}"
                : $"    cb[{ci}] STD uoff=0x{ce.UoffsetStart:X} coffMod=0x{ce.CoffsetStartMod256K:X} " +
                  $"clenEvenM1={ce.ClenEvenMinus1} even={ce.Even} odd={ce.Odd} kde={ce.KdePredictor} " +
                  $"shuf={ce.ShuffleIdx} flagHint=0x{ce.DecodeFlagHint:X2} raw8=0x{ce.Raw[8]:X2}");
        }

        // Direct per-block decode with the KNOWN boundary-table flags — isolates layout errors
        // from the reader's flag-guessing heuristic.
        Console.WriteLine("== Per-block decode with true flags ==");
        {
            var imgFs = result.Image is { } imgBytes
                ? (Stream)new MemoryStream(imgBytes, writable: false)
                : new FileStream(result.ImagePath!, FileMode.Open, FileAccess.Read, FileShare.Read);
            using (imgFs)
            foreach (var p in result.Placements)
            {
                if (p.StoreRaw || p.Blocks is not { Count: > 0 } blks) continue;
                var want = expectBytes.FirstOrDefault(kv => kv.Value.LongLength == p.UncompressedSize);
                long onDisk = p.OnDiskOffset;
                int bad = 0, okc = 0;
                for (int bi = 0; bi < blks.Count; bi++)
                {
                    var b = blks[bi];
                    var srcb = new byte[b.CompressedSize];
                    imgFs.Position = onDisk;
                    imgFs.ReadExactly(srcb);
                    var dst = new byte[b.UncompressedSize];
                    var st = LibProsperoPkg.PFS.Compression.Oodle.KrakenDecoder.DecodeBlock(
                        srcb, b.Flags, b.FirstChunkCompressedSize, dst);
                    bool ok = st == LibProsperoPkg.PFS.Compression.Oodle.KrakenDecodeStatus.Success;
                    if (ok && want.Value is not null)
                        ok = dst.AsSpan().SequenceEqual(
                            want.Value.AsSpan(bi * 0x40000, b.UncompressedSize));
                    if (ok) okc++;
                    else
                    {
                        bad++;
                        if (bad <= 3)
                            Console.WriteLine($"    plc@{p.LogicalOffset:X} blk{bi}: comp=0x{b.CompressedSize:X} " +
                                              $"uncomp=0x{b.UncompressedSize:X} flags=0x{b.Flags:X2} " +
                                              $"firstComp=0x{b.FirstChunkCompressedSize:X} -> {st} " +
                                              $"{(st.ToString() == "Success" ? "WRONG BYTES" : "")}");
                    }
                    onDisk += b.CompressedSize;
                }
                var flagSet = blks.GroupBy(b => b.Flags).Select(g => $"0x{g.Key:X2}×{g.Count()}");
                Console.WriteLine($"    plc@{p.LogicalOffset:X}: {okc}/{blks.Count} blocks ok with true flags " +
                                  $"flags=[{string.Join(",", flagSet)}]");
            }
        }

        Console.WriteLine("== Reconstructing mount ==");
        string mountPath = Path.Combine(tmp, "mount.bin");
        IMemoryReader innerView = result.Image is { } img
            ? new LibProsperoPkg.Util.StreamReader(new MemoryStream(img, writable: false), 0, takeOwnership: true)
            : new LibProsperoPkg.Util.StreamReader(new FileStream(result.ImagePath!, FileMode.Open, FileAccess.Read,
                                              FileShare.Read, 1 << 20), 0, takeOwnership: true);
        long sbOff;
        using (var mfs = new FileStream(mountPath, FileMode.Create, FileAccess.ReadWrite,
                   FileShare.None, 1 << 20, FileOptions.RandomAccess))
        {
            // No explicit hints: the STD byte-8 flag hints embedded in the serialized naps must
            // carry the decode by themselves (round-trip through bytes).
            sbOff = ProsperoPs5InnerImageReader.ReconstructMountToStream(innerView, naps, mfs);
            Console.WriteLine($"  mount: {mfs.Length:N0} bytes, superblock @0x{sbOff:X}");

            var probe = new byte[16];
            mfs.Position = sbOff;
            mfs.ReadExactly(probe);
            Console.WriteLine($"  bytes @sbOff: {Convert.ToHexString(probe)}");

            var tree = ProsperoPs5InnerImageReader.ReadFileTree(new LibProsperoPkg.Util.StreamReader(mfs), sbOff);
            Console.WriteLine($"  inner files: {tree.Count}");
            foreach (var f in tree)
                Console.WriteLine($"    /{f.Path} off=0x{f.LogicalOffset:X} size={f.Size:N0}");

            // Byte-for-byte compare each extracted file against its source.
            int fail = 0;
            var buf = new byte[1 << 20];
            foreach (var f in tree)
            {
                string rel = "/" + f.Path.Replace('\\', '/');
                if (!expectBytes.TryGetValue(rel, out var want))
                {
                    Console.WriteLine($"    !! unexpected inner file {rel}");
                    fail++;
                    continue;
                }
                if (f.Size != want.LongLength)
                {
                    Console.WriteLine($"    !! SIZE MISMATCH {rel}: {f.Size} vs {want.LongLength}");
                    fail++;
                    continue;
                }
                mfs.Position = (long)f.LogicalOffset;
                using var sha = SHA256.Create();
                long remaining = f.Size;
                while (remaining > 0)
                {
                    int n = mfs.Read(buf, 0, (int)Math.Min(buf.Length, remaining));
                    if (n <= 0) break;
                    sha.TransformBlock(buf, 0, n, null, 0);
                    remaining -= n;
                }
                sha.TransformFinalBlock(Array.Empty<byte>(), 0, 0);
                byte[] wantHash = SHA256.HashData(want);
                if (!sha.Hash!.AsSpan().SequenceEqual(wantHash))
                {
                    // Locate the first differing byte to see if corruption is per-block or total.
                    long fileOff = (long)f.LogicalOffset;
                    long firstDiff = -1; long matchLen = 0;
                    var rb = new byte[0x40000];
                    for (long o = 0; o < want.LongLength; o += rb.Length)
                    {
                        int n = (int)Math.Min(rb.Length, want.LongLength - o);
                        mfs.Position = fileOff + o;
                        mfs.ReadExactly(rb, 0, n);
                        for (int i = 0; i < n; i++)
                        {
                            if (rb[i] != want[o + i])
                            { firstDiff = o + i; break; }
                        }
                        if (firstDiff >= 0) break;
                        matchLen += n;
                    }
                    Console.WriteLine($"    !! CONTENT MISMATCH {rel}: first diff @0x{firstDiff:X} " +
                                      $"(matched {matchLen:N0} bytes, {matchLen / 0x40000} full ublocks)");
                    fail++;
                }
                else Console.WriteLine($"    ok {rel}");
            }
            innerView.Dispose();
            Console.WriteLine(fail == 0 ? "NAPS ROUND-TRIP PASSED" : $"NAPS ROUND-TRIP FAILED ({fail})");
            return fail;
        }
    }
    finally { try { Directory.Delete(tmp, recursive: true); } catch { } }
}

// ---------- pkgbuild: full Build -> Extract -> diff round-trip ----------
static async Task<int> PkgBuildVerifyAsync(long srcBytes)
{
    string tmp = Path.Combine(Path.GetTempPath(), "pkgbuild-" + Guid.NewGuid().ToString("N"));
    string src = Path.Combine(tmp, "src");
    string outDir = Path.Combine(tmp, "pkg");
    string extDir = Path.Combine(tmp, "ext");
    Directory.CreateDirectory(src);
    try
    {
        // Synthetic source tree: compressible + incompressible + small files + sce_sys.
        var rng = new Random(777);
        Directory.CreateDirectory(Path.Combine(src, "sce_sys"));
        File.WriteAllText(Path.Combine(src, "sce_sys", "param.json"), "{}");
        Directory.CreateDirectory(Path.Combine(src, "assets", "levels"));

        var tile = new byte[0x1000];
        rng.NextBytes(tile);
        long written = 0;
        using (var fs = new FileStream(Path.Combine(src, "assets", "big.dat"), FileMode.Create))
            while (written < srcBytes / 2) { fs.Write(tile); written += tile.Length; }

        var rand = new byte[srcBytes / 4];
        rng.NextBytes(rand);
        File.WriteAllBytes(Path.Combine(src, "assets", "random.bin"), rand);

        var mixed = new byte[srcBytes / 4];
        for (long i = 0; i < mixed.LongLength; i++) mixed[i] = i % 3 == 0 ? rand[(int)(i % rand.LongLength)] : (byte)(i >> 9);
        File.WriteAllBytes(Path.Combine(src, "assets", "levels", "mixed.bin"), mixed);

        File.WriteAllBytes(Path.Combine(src, "tiny.dat"), new byte[] { 1, 2, 3, 42 });

        Console.WriteLine($"== Building package from {src} ==");
        var opts = new ProsperoBuildOptions
        {
            SourceFolder = src,
            OutputFolder = outDir,
            ContentId = "UP9000-PPSA00000_00-HOMEBREW00000000",
            TitleId = "PPSA00000",
            Title = "pkgbuild test",
            LicenseFree = true,
        };
        var build = ProsperoPackageBuilder.Build(opts, m => Console.WriteLine($"  | {m}"));
        Console.WriteLine($"  built: {build.OutputPath} ({new FileInfo(build.OutputPath).Length:N0} bytes)");
        foreach (var w in build.Warnings) Console.WriteLine($"  warning: {w}");

        Console.WriteLine("== Extracting via ProsperoPackageExtractor ==");
        var manifest = ProsperoPackageExtractor.Extract(build.OutputPath, extDir, opts.Passcode,
            m => Console.WriteLine($"  | {m}"));
        Console.WriteLine($"  extracted {manifest.ExtractedFileCount} files");

        Console.WriteLine("== Diffing source vs extracted ==");
        var srcFiles = Directory.GetFiles(src, "*", SearchOption.AllDirectories)
            .Select(p => Path.GetRelativePath(src, p).Replace('\\', '/'))
            .ToList();
        int matched = 0, missing = 0, mismatched = 0;
        foreach (var rel in srcFiles)
        {
            // fake-signing rewrites ELF modules then restores them — our tree has none.
            var extPath = Path.Combine(extDir, rel.Replace('/', Path.DirectorySeparatorChar));
            if (!File.Exists(extPath))
            {
                // Builder-managed: param.json and every sce_sys file with a CNT entry id are stored
                // as package metadata entries (materialized to sce_sys/ on install), not inner files.
                if (rel == "sce_sys/param.json" ||
                    (rel.StartsWith("sce_sys/") &&
                     LibProsperoPkg.PKG.ProsperoCntEntryNames.NameToId.ContainsKey(rel["sce_sys/".Length..])))
                { Console.WriteLine($"  (CNT entry, not packed: {rel})"); continue; }
                Console.WriteLine($"  MISSING {rel}");
                missing++;
                continue;
            }
            long sLen = new FileInfo(Path.Combine(src, rel.Replace('/', Path.DirectorySeparatorChar))).Length;
            long eLen = new FileInfo(extPath).Length;
            if (sLen != eLen) { Console.WriteLine($"  SIZE MISMATCH {rel}: {sLen} vs {eLen}"); mismatched++; continue; }
            byte[] h1 = SHA256.HashData(File.ReadAllBytes(Path.Combine(src, rel.Replace('/', Path.DirectorySeparatorChar))));
            byte[] h2 = SHA256.HashData(File.ReadAllBytes(extPath));
            if (!h1.AsSpan().SequenceEqual(h2)) { Console.WriteLine($"  CONTENT MISMATCH {rel}"); mismatched++; continue; }
            matched++;
        }
        Console.WriteLine($"  matched={matched} missing={missing} mismatched={mismatched}");
        int fail = missing + mismatched;
        Console.WriteLine(fail == 0 ? "PKGBUILD ROUND-TRIP PASSED" : $"PKGBUILD ROUND-TRIP FAILED ({fail})");
        return fail;
    }
    finally { try { Directory.Delete(tmp, recursive: true); } catch { } }
}

// ---------- shared: open the data-first outer PFS with on-demand decryption ----------
// (OuterDecReader is declared at the end of the file — top-level programs require
//  type declarations after all top-level statements.)

static (ProsperoPfsReader outer, ProsperoPfsReader.File pfsImg, ProsperoPfsReader.File naps)
    OpenOuter(string pkgPath, string passcode, out long pfsSizeOut)
{
    using var probe = File.OpenRead(pkgPath);
    var fih = new byte[0x100];
    probe.ReadExactly(fih);
    long pfsOffset = (long)BinaryPrimitives.ReadUInt64LittleEndian(fih.AsSpan(0x10));
    long pfsSize = (long)BinaryPrimitives.ReadUInt64LittleEndian(fih.AsSpan(0x18));
    long sbAbs = (long)BinaryPrimitives.ReadUInt64LittleEndian(fih.AsSpan(0x20));
    pfsSizeOut = pfsSize;

    const int BS = 0x10000;
    long sbRel = sbAbs - pfsOffset;
    long total = pfsSize / BS;
    long sbBlock = sbRel / BS;
    Console.WriteLine($"  pfsOffset=0x{pfsOffset:X} pfsSize=0x{pfsSize:X} sbAbs=0x{sbAbs:X} sbBlock={sbBlock} totalBlocks={total}");

    // Persistent stream for the reader (must outlive this method).
    var pkgStream = new FileStream(pkgPath, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
    var backing = new LibProsperoPkg.Util.StreamReader(pkgStream, pfsOffset);

    var sbBuf = new byte[0x400];
    backing.Read(sbRel, sbBuf, 0, sbBuf.Length);
    var sb = ProsperoPfsHeader.ReadFromStream(new MemoryStream(sbBuf));
    byte[] seed = sb.Seed;
    Console.WriteLine($"  seed={Convert.ToHexString(seed)} dinodeCount={sb.DinodeCount} ndblock={sb.Ndblock}");

    string contentId = ProsperoPackageExtractor.Inspect(pkgPath).ContentId;
    if (string.IsNullOrEmpty(contentId) || contentId.Length != 36)
        contentId = Path.GetFileName(pkgPath).Split("-A")[0];
    byte[] ekpfs = Crypto.ComputeKeys(contentId, passcode, 1, useSha3: true);
    var (tweak, data) = Crypto.PfsGenEncKey(ekpfs, seed, true);

    // Classic (superblock-first) outer PFS: try first — many real pkgs use it.
    try
    {
        var classic = new ProsperoPfsReader(backing, 0, ekpfs);
        var cf = classic.GetAllFiles().ToList();
        if (cf.Count > 0)
        {
            Console.WriteLine($"  classic outer PFS: {string.Join(", ", cf.Select(f => $"{f.name}(sz={f.size},csz={f.compressed_size})"))}");
            return (classic, classic.GetFile("pfs_image.dat")!, classic.GetFile("naps_pkg_layout.dat")!);
        }
    }
    catch (Exception ex) { Console.WriteLine($"  classic outer failed: {ex.Message}"); }

    var kinds = new ProsperoOuterBlockKind[(int)total];
    Array.Fill(kinds, ProsperoOuterBlockKind.Signed);
    kinds[sbBlock] = ProsperoOuterBlockKind.Plaintext;
    var dec = new OuterDecReader(backing, BS, tweak, data, kinds);

    var probeReader = new ProsperoPfsReader(dec, 0, null, null, null, sbBlock * BS, skipDecryption: true);
    var files = probeReader.GetAllFiles().ToList();
    Console.WriteLine($"  outer files: {string.Join(", ", files.Select(f => $"{f.name}(sz={f.size},csz={f.compressed_size},off=0x{f.offset:X})"))}");

    var pfsImg = files.FirstOrDefault(f => f.name == "pfs_image.dat")
        ?? throw new Exception("pfs_image.dat not found");
    var naps = files.FirstOrDefault(f => f.name == "naps_pkg_layout.dat")
        ?? throw new Exception("naps_pkg_layout.dat not found");

    long startBlk = pfsImg.offset / BS;
    long endBlk = sbBlock;
    foreach (var f in files)
    {
        long fb = f.offset / BS;
        if (fb > startBlk && fb < endBlk) endBlk = fb;
    }
    Console.WriteLine($"  pfs_image.dat blocks [{startBlk}..{endBlk}) marked Data");
    for (long b = startBlk; b < endBlk && b < total; b++)
        kinds[(int)b] = ProsperoOuterBlockKind.Data;

    // Final reader over corrected kinds.
    var reader = new ProsperoPfsReader(dec, 0, null, null, null, sbBlock * BS, skipDecryption: true);
    return (reader, reader.GetFile("pfs_image.dat")!, reader.GetFile("naps_pkg_layout.dat")!);
}

// ---------- diag ----------
static int Diag(string pkgPath, string passcode)
{
    var (outer, pfsImg, napsFile) = OpenOuter(pkgPath, passcode, out long pfsSize);
    _ = outer;

    byte[] napsBytes = new byte[napsFile.size];
    napsFile.GetView().Read(0, napsBytes, 0, napsBytes.Length);
    var doc = ProsperoNapsLayout.Parse(napsBytes);
    Console.WriteLine($"  naps: fileOffsets={doc.FileOffsets.Count} cblockInfos={doc.CblockInfos.Count}");
    int kraken = doc.CblockInfos.Count(e => !e.IsRunBase && e.KdePredictor == 2);
    int runBase = doc.CblockInfos.Count(e => e.IsRunBase);
    Console.WriteLine($"  cblocks: kraken={kraken} raw={doc.CblockInfos.Count - kraken - runBase} runBase={runBase}");

    var rawOffs = doc.FileOffsets.Select(f => (long)f.UncompressedOffsetStart).ToList();
    Console.WriteLine($"  fidx first5: {string.Join(", ", rawOffs.Take(5).Select(v => $"0x{v:X}"))}");
    Console.WriteLine($"  fidx last5: {string.Join(", ", rawOffs.TakeLast(5).Select(v => $"0x{v:X}"))}");

    long mountSize = pfsImg.compressed_size > pfsImg.size ? pfsImg.compressed_size : 0;
    if (mountSize <= 0)
        mountSize = rawOffs.Where(v => v > 0 && (v & 0xFFFF) == 0).DefaultIfEmpty(0).Max();
    var boundaries = rawOffs.Where(v => v > 0 && v <= mountSize).Distinct().OrderBy(v => v).ToArray();
    long metaBase = boundaries.LastOrDefault(v => v < mountSize);
    Console.WriteLine($"  pfs_image.dat: size={pfsImg.size} compressed_size={pfsImg.compressed_size} mountSize=0x{mountSize:X} metaBase=0x{metaBase:X}");

    var view = pfsImg.GetView();
    // Sanity: read the first file bytes and naps region.
    var head = new byte[32];
    view.Read(0, head, 0, 32);
    Console.WriteLine($"  inner head: {Convert.ToHexString(head)}");

    string mountPath = Path.Combine(Path.GetTempPath(), "diag-mount.bin");
    try
    {
        using (var mfs = new FileStream(mountPath, FileMode.Create, FileAccess.ReadWrite, FileShare.None, 1 << 20))
        {
            int issues = 0;
            ProsperoPs5InnerImageReader.DecodeDiag += msg =>
            {
                issues++;
                if (issues <= 30) Console.WriteLine($"  !! {msg}");
            };
            var sw = System.Diagnostics.Stopwatch.StartNew();
            long sbOff = ProsperoPs5InnerImageReader.ReconstructMountToStream(view, napsBytes, mfs, mountSize);
            Console.WriteLine($"  mount reconstructed to {mountPath} ({mfs.Length:N0} bytes) in {sw.Elapsed:mm\\:ss}, sbOff=0x{sbOff:X}");
            if (issues > 0) Console.WriteLine($"  !! {issues} cblock decode issues");

            var probe = new byte[16];
            mfs.Position = sbOff;
            mfs.ReadExactly(probe);
            Console.WriteLine($"  bytes @sbOff: {Convert.ToHexString(probe)}");

            // Debug: dump superblock fields + first inodes before tree walk.
            var sbBuf = new byte[0x40];
            mfs.Position = sbOff;
            mfs.ReadExactly(sbBuf);
            int blkSz = BitConverter.ToInt32(sbBuf, 0x20);
            long inoCnt = BitConverter.ToInt64(sbBuf, 0x30);
            Console.WriteLine($"  sb: blockSize=0x{blkSz:X} inodeCount={inoCnt}");
            var ino = new byte[0xA8];
            for (long k = 0; k < Math.Min(inoCnt, 6); k++)
            {
                mfs.Position = sbOff + blkSz + k * 0xA8;
                mfs.ReadExactly(ino);
                Console.WriteLine($"  ino[{k}]: mode=0x{BitConverter.ToUInt16(ino, 0):X4} " +
                                  $"size=0x{BitConverter.ToInt64(ino, 8):X} off=0x{BitConverter.ToUInt64(ino, 0x60):X} " +
                                  $"parent={BitConverter.ToUInt32(ino, 0x6C)}");
            }

            var tree = ProsperoPs5InnerImageReader.ReadFileTree(new LibProsperoPkg.Util.StreamReader(mfs), sbOff);
            Console.WriteLine($"  inner files: {tree.Count}");
            foreach (var f in tree.Take(10))
                Console.WriteLine($"    {f.Path} off=0x{f.LogicalOffset:X} size={f.Size}");
            if (tree.Count > 10) Console.WriteLine($"    ... +{tree.Count - 10} more");
        }
    }
    finally { try { File.Delete(mountPath); } catch { } }
    return 0;
}

// ---------- verify ----------
static async Task<int> VerifyAsync(string pkgPath, string srcFolder, string outDir, string passcode)
{
    int failures = 0;

    Console.WriteLine($"== Validating {pkgPath} ==");
    var report = ProsperoPkgValidator.Validate(pkgPath);
    foreach (var c in report.Checks)
        Console.WriteLine($"  [{c.Status}] {c.Name}: {c.Detail}");
    if (!report.Accepted) { Console.WriteLine("  VALIDATION FAILED"); failures++; }

    var info = ProsperoPackageExtractor.Inspect(pkgPath);
    Console.WriteLine($"  type={info.PackageType} retail={info.IsRetail} contentId={info.ContentId}");
    Console.WriteLine($"  pfs offset=0x{info.PfsImageOffset:X} size=0x{info.PfsImageSize:X} outerEncrypted={info.OuterEncrypted}");

    Console.WriteLine("== Extracting package (full round-trip) ==");
    var manifest = ProsperoPackageExtractor.Extract(pkgPath, outDir, passcode,
        m => Console.WriteLine($"  | {m}"));
    Console.WriteLine($"  extracted {manifest.ExtractedFileCount} files, outer files={manifest.OuterFileCount}, " +
        $"innerCompressed={manifest.InnerImageCompressed}, ekpfs={manifest.EkpfsFingerprint}");

    Console.WriteLine("== Diffing extracted files against source tree ==");
    var srcFull = Path.GetFullPath(srcFolder);
    var srcFiles = Directory.GetFiles(srcFull, "*", SearchOption.AllDirectories)
        .Select(p => Path.GetRelativePath(srcFull, p).Replace('\\', '/'))
        .ToList();
    var outFull = Path.GetFullPath(outDir);
    var outFiles = Directory.GetFiles(outFull, "*", SearchOption.AllDirectories)
        .Select(p => Path.GetRelativePath(outFull, p).Replace('\\', '/'))
        .ToHashSet(StringComparer.OrdinalIgnoreCase);

    int matched = 0, missing = 0, mismatched = 0;
    foreach (var rel in srcFiles)
    {
        if (!outFiles.Contains(rel))
        {
            // Builder-managed: param.json and every sce_sys file with a CNT entry id is stored as a
            // package metadata entry (materialized to sce_sys/ on install), not an inner file.
            if (rel == "sce_sys/param.json" ||
                (rel.StartsWith("sce_sys/") &&
                 LibProsperoPkg.PKG.ProsperoCntEntryNames.NameToId.ContainsKey(rel["sce_sys/".Length..])))
            { Console.WriteLine($"  (CNT entry, not packed: {rel})"); continue; }
            Console.WriteLine($"  MISSING in pkg: {rel}");
            missing++;
            continue;
        }
        var srcPath = Path.Combine(srcFull, rel.Replace('/', Path.DirectorySeparatorChar));
        var outPath = Path.Combine(outFull, rel.Replace('/', Path.DirectorySeparatorChar));
        long sLen = new FileInfo(srcPath).Length;
        long oLen = new FileInfo(outPath).Length;
        if (sLen != oLen)
        {
            Console.WriteLine($"  SIZE MISMATCH {rel}: {sLen} vs {oLen}");
            mismatched++;
            continue;
        }
        byte[] h1, h2;
        using (var s = File.OpenRead(srcPath)) h1 = await SHA256.HashDataAsync(s);
        using (var s = File.OpenRead(outPath)) h2 = await SHA256.HashDataAsync(s);
        if (!h1.AsSpan().SequenceEqual(h2))
        {
            Console.WriteLine($"  CONTENT MISMATCH {rel}");
            mismatched++;
            continue;
        }
        matched++;
    }
    Console.WriteLine($"  matched={matched} missing={missing} mismatched={mismatched}");

    var srcSet = srcFiles.ToHashSet(StringComparer.OrdinalIgnoreCase);
    var extra = outFiles.Where(f => !srcSet.Contains(f)).ToList();
    if (extra.Count > 0)
        Console.WriteLine($"  extra pkg-only files ({extra.Count}): {string.Join(", ", extra.Take(10))}");

    failures += missing + mismatched;
    Console.WriteLine(failures == 0 ? "PACKAGE VERIFIED" : $"{failures} PROBLEMS FOUND");
    return failures;
}

// On-demand per-block AES-XTS decryption view over a data-first outer PFS image (mirror of the
// library's internal reader; kept here so the diagnostic path is independent).
// ---------- inodeoff: inode LogicalOffset must survive >4GiB (u64 @0x60, db[] @0x68) ----------
static int InodeOffsetCheck()
{
    var meta = new ProsperoPs5InnerMetadata(1700000000L, 0);
    const ulong want = 0x27EAA0000UL; // >4GiB metadata-region offset like the real PPSA10737 image
    var nodes = new List<ProsperoPs5MetaNode>
    {
        new() { Name = "", Inode = 0, IsDirectory = true, Mode = 0x416d, Size = ProsperoPs5InnerMetadata.BlockSize,
                LogicalOffset = want, ParentInode = -1 },
    };
    byte[] mp = meta.Build(nodes, 0x28000, new List<byte[]> { Array.Empty<byte>() });
    ulong lo = BitConverter.ToUInt64(mp, ProsperoPs5InnerMetadata.BlockSize + 0x60);
    if (lo != want)
    {
        Console.WriteLine($"FAIL: inode LogicalOffset serialized as 0x{lo:X} (want 0x{want:X})");
        return 1;
    }
    Console.WriteLine($"inode LogicalOffset >4GiB round-trip OK: 0x{lo:X}");
    return 0;
}

// ---------- realbuild: build a package from a real source folder (same options as the client) ----------
static int RealBuild(string srcFolder, string outDir)
{
    Directory.CreateDirectory(outDir);
    // Prefer the source tree's own identity: a pkg whose CNT content-id disagrees
    // with param.json is rejected by the console installer.
    string contentId = "UP9000-PPSA00000_00-HOMEBREW00000000";
    string titleId = "PPSA00000";
    string title = "Homebrew App";
    string version = "01.00";
    string paramPath = Path.Combine(srcFolder, "sce_sys", "param.json");
    if (File.Exists(paramPath))
    {
        using var doc = System.Text.Json.JsonDocument.Parse(File.ReadAllText(paramPath));
        var root = doc.RootElement;
        if (root.TryGetProperty("contentId", out var c)) contentId = c.GetString() ?? contentId;
        if (root.TryGetProperty("titleId", out var t)) titleId = t.GetString() ?? titleId;
        if (root.TryGetProperty("contentVersion", out var v)) version = v.GetString() ?? version;
        if (root.TryGetProperty("localizedParameters", out var lp) &&
            lp.TryGetProperty("en-US", out var en) &&
            en.TryGetProperty("titleName", out var tn))
            title = tn.GetString() ?? title;
        Console.WriteLine($"param.json identity: contentId={contentId} titleId={titleId} version={version} title={title}");
    }
    var opts = new ProsperoBuildOptions
    {
        Mode = ProsperoPackageMode.Application,
        OutputFormat = ProsperoOutputFormat.DebugImage,
        SourceFolder = srcFolder,
        OutputFolder = outDir,
        ContentId = contentId,
        TitleId = titleId,
        Title = title,
        Version = version,
        FakeSignSelfModules = true,
        ApplicationType = ProsperoApplicationType.FreemiumApp,
    };
    var result = ProsperoPackageBuilder.Build(opts, m => Console.WriteLine($"  | {m}"));
    if (result == null || string.IsNullOrEmpty(result.OutputPath)) { Console.WriteLine("BUILD FAILED"); return 1; }
    Console.WriteLine($"built: {result.OutputPath} ({new FileInfo(result.OutputPath).Length:N0} bytes)");
    foreach (var w in result.Warnings) Console.WriteLine($"  warning: {w}");
    return 0;
}

// ---------- homebrew: license-free debug fPKG via ProsperoHomebrewPackager ----------
static int HomebrewBuild(string homebrewFolder, string outDir)
{
    var opts = new ProsperoHomebrewPackageOptions
    {
        HomebrewFolder = homebrewFolder,
        OutputFolder = outDir,
        KeepStaging = false,
    };
    var result = ProsperoHomebrewPackager.Package(opts, m => Console.WriteLine($"  | {m}"));
    if (result == null || string.IsNullOrEmpty(result.OutputPath)) { Console.WriteLine("BUILD FAILED"); return 1; }
    Console.WriteLine($"built: {result.OutputPath} ({new FileInfo(result.OutputPath).Length:N0} bytes)");
    Console.WriteLine($"launch ready: {(result.LaunchReadiness.IsLaunchReady ? "yes" : "NO")}");
    foreach (var i in result.LaunchReadiness.Issues) Console.WriteLine($"  issue: {i}");
    foreach (var w in result.Warnings) Console.WriteLine($"  warning: {w}");
    return result.LaunchReadiness.IsLaunchReady ? 0 : 1;
}

// ---------- cntdump: list container header + entries ----------
static int CntDump(string pkgPath)
{
    var pkg = ProsperoPkgReader.Read(pkgPath);
    Console.WriteLine($"type={pkg.Type}");
    if (pkg.Header is { } h)
        Console.WriteLine($"flags=0x{h.Flags:X8} entries={h.EntryCount} scEntries={h.ScEntryCount} " +
                          $"contentId={h.ContentId} drm=0x{h.DrmType:X} ctype=0x{h.ContentType:X} cflags=0x{h.ContentFlags:X8}");
    if (pkg.Fih is { } f)
        Console.WriteLine($"FIH: signed=0x{f.SignedByte:X2} fmt={f.FormatVersion} pfs@0x{f.PfsImageOffset:X}+0x{f.PfsImageSize:X} cnt@0x{f.EmbeddedCntOffset:X}");
    foreach (var e in pkg.Entries)
        Console.WriteLine($"  id=0x{e.RawId:X4} name={e.Name ?? "(unnamed)"} off=0x{e.DataOffset:X} size=0x{e.DataSize:X} " +
                          $"enc={(e.Encrypted ? 1 : 0)} keyIdx={e.KeyIndex} f1=0x{e.Flags1:X8} f2=0x{e.Flags2:X8}");
    return 0;
}



// ---------- fself: wrap a raw ELF module in the PS5 SELF container ----------
static int FselfWrap(string inElf, string outFile)
{
    var elf = File.ReadAllBytes(inElf);
    var fself = LibProsperoPkg.Content.ProsperoFself.MakeFself(elf);
    File.WriteAllBytes(outFile, fself);
    Console.WriteLine($"fself: {inElf} ({elf.Length} B) -> {outFile} ({fself.Length} B), magic={BinaryPrimitives.ReadUInt32LittleEndian(fself):X8}");
    return 0;
}


sealed class OuterDecReader : IMemoryReader
{
    private readonly IMemoryReader _src;
    private readonly int _bs;
    private readonly XtsBlockTransform _xts;
    public ProsperoOuterBlockKind[] Kinds;
    private readonly byte[] _scratch;
    private int _cached = -1;

    public OuterDecReader(IMemoryReader src, int bs, byte[] tweak, byte[] data, ProsperoOuterBlockKind[] kinds)
    {
        _src = src; _bs = bs; Kinds = kinds; _scratch = new byte[bs];
        _xts = new XtsBlockTransform(data, tweak);
    }
    public void Dispose() => _xts.Dispose();
    public void Read(long pos, byte[] buf, int offset, int count)
    {
        long end = pos + count;
        while (pos < end)
        {
            int blk = (int)(pos / _bs);
            if (blk != _cached)
            {
                _src.Read((long)blk * _bs, _scratch, 0, _bs);
                var kind = blk < Kinds.Length ? Kinds[blk] : ProsperoOuterBlockKind.Signed;
                if (kind != ProsperoOuterBlockKind.Plaintext)
                    _xts.DecryptSector(_scratch,
                        ProsperoOuterPfsSignature.BlockSector(blk, kind == ProsperoOuterBlockKind.Signed));
                _cached = blk;
            }
            int inBlk = (int)(pos - (long)blk * _bs);
            int n = (int)Math.Min(end - pos, _bs - inBlk);
            Array.Copy(_scratch, inBlk, buf, offset, n);
            pos += n; offset += n;
        }
    }
}