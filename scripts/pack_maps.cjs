// Packs rom/cache/client/maps/{m,l}<X>_<Z> (currently ~830 separate small files) into one combined
// rom/cache/client/maps.dat for PS2 - see client.c's ps2_map_archive_* functions for the reader.
// Real PS2 USB mass storage racks up disproportionate FAT/directory-lookup overhead per discrete
// file open on slow media; one archive read via seeks (after one open) needs far fewer of those.
//
// Format (all integers little-endian, matching the EE's mips64r5900el target):
//   header: char magic[4] = "MAPZ"; uint32 entry_count;
//   index:  entry_count * { uint8 kind ('m'/'l'); uint8 mapsquareX; uint8 mapsquareZ;
//                            uint8 reserved; uint32 offset; uint32 length } (12 bytes each)
//   data:   concatenated raw file bytes, in index order, starting right after the index
//
// Usage: node scripts/pack_maps.js <maps-dir> <output-file>
// Every other platform keeps reading the individual files unchanged - this archive is PS2-only.

const fs = require('fs');
const path = require('path');

const [, , mapsDir, outFile] = process.argv;
if (!mapsDir || !outFile) {
    console.error('Usage: node scripts/pack_maps.js <maps-dir> <output-file>');
    process.exit(1);
}

const names = fs.readdirSync(mapsDir).filter((f) => /^[ml]\d+_\d+$/.test(f));
names.sort();

const entries = names.map((name) => {
    const match = name.match(/^([ml])(\d+)_(\d+)$/);
    const [, kind, xStr, zStr] = match;
    const x = parseInt(xStr, 10);
    const z = parseInt(zStr, 10);
    if (x > 255 || z > 255) {
        throw new Error(`mapsquare coordinate out of uint8 range in ${name}: ${x},${z}`);
    }
    const data = fs.readFileSync(path.join(mapsDir, name));
    return { kind, x, z, data, name };
});

const HEADER_SIZE = 8;
const INDEX_ENTRY_SIZE = 12;
const indexSize = entries.length * INDEX_ENTRY_SIZE;

const header = Buffer.alloc(HEADER_SIZE);
header.write('MAPZ', 0, 'ascii');
header.writeUInt32LE(entries.length, 4);

const index = Buffer.alloc(indexSize);
let dataOffset = HEADER_SIZE + indexSize;
let indexPos = 0;
for (const entry of entries) {
    index.writeUInt8(entry.kind.charCodeAt(0), indexPos + 0);
    index.writeUInt8(entry.x, indexPos + 1);
    index.writeUInt8(entry.z, indexPos + 2);
    index.writeUInt8(0, indexPos + 3); // reserved
    index.writeUInt32LE(dataOffset, indexPos + 4);
    index.writeUInt32LE(entry.data.length, indexPos + 8);
    dataOffset += entry.data.length;
    indexPos += INDEX_ENTRY_SIZE;
}

const out = Buffer.concat([header, index, ...entries.map((e) => e.data)]);
fs.writeFileSync(outFile, out);
console.log(`Packed ${entries.length} map files (${out.length} bytes) -> ${outFile}`);
