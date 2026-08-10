import { createHash } from 'node:crypto';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { inflateSync } from 'node:zlib';

const PNG_SIGNATURE = '89504e470d0a1a0a';
const DEFAULT_OUTPUT = resolve(
  import.meta.dirname,
  '../src/windows/src/generated_packed_fx_texture_data.inc',
);
const TEXTURE_DIRECTORY = 'Assets/Imported/FX_Touch/Textures';
const SOURCE_TEXTURES = Object.freeze([
  Object.freeze({
    id: 'centerDisk',
    file: 'FX_TEX_Circle_01.png',
    sha256: 'f8675e0a16959eda829ae7d516fb609a4d45434520866466d04b78745f0badd2',
    width: 512,
    height: 512,
    layout: 'rgba8-srgb',
  }),
  Object.freeze({
    id: 'dissolveRing',
    file: 'FX_TEX_Grad_Ring3.png',
    sha256: '517236c7c818a3715f8ba03ec316853bec92ffd6e032b8e5d21daedffc809684',
    width: 256,
    height: 128,
    layout: 'r8-unorm',
  }),
  Object.freeze({
    id: 'triangleAtlas',
    file: 'FX_TEX_Triangle_02_1.png',
    sha256: '0eb35fda5710344bedb5713b0b197b1c190ec4d8851ef8dd916b4e17de39a068',
    width: 256,
    height: 128,
    layout: 'rgba8-srgb',
  }),
  Object.freeze({
    id: 'trail',
    file: 'FX_TEX_Trail_03.png',
    sha256: '16001511757e7007f43db9613e24144b5e8d726239de0262f55d9e14c0f00feb',
    width: 512,
    height: 512,
    layout: 'rgba8-srgb',
  }),
]);

function usage()
{
  throw new Error(
    'Usage: node tools/generate-packed-fx-textures.mjs ' +
      '--project <Unity project root> [--output <generated include>]',
  );
}

function parseArguments(argumentsList)
{
  let projectRoot = '';
  let outputPath = DEFAULT_OUTPUT;

  for (let index = 0; index < argumentsList.length; index++)
  {
    const argument = argumentsList[index];

    if (argument === '--project' && index + 1 < argumentsList.length)
    {
      projectRoot = resolve(argumentsList[++index]);
    }
    else if (argument === '--output' && index + 1 < argumentsList.length)
    {
      outputPath = resolve(argumentsList[++index]);
    }
    else
    {
      usage();
    }
  }

  if (!projectRoot)
  {
    usage();
  }

  return { outputPath, projectRoot };
}

function sha256(bytes)
{
  return createHash('sha256').update(bytes).digest('hex');
}

function paeth(left, above, upperLeft)
{
  const prediction = left + above - upperLeft;
  const leftDistance = Math.abs(prediction - left);
  const aboveDistance = Math.abs(prediction - above);
  const upperLeftDistance = Math.abs(prediction - upperLeft);

  if (leftDistance <= aboveDistance && leftDistance <= upperLeftDistance)
  {
    return left;
  }
  if (aboveDistance <= upperLeftDistance)
  {
    return above;
  }
  return upperLeft;
}

function decodePngRgba(source, expectedWidth, expectedHeight)
{
  if (source.subarray(0, 8).toString('hex') !== PNG_SIGNATURE)
  {
    throw new Error('Texture input is not a PNG');
  }

  let width = 0;
  let height = 0;
  let bitDepth = 0;
  let colorType = 0;
  let interlace = 0;
  let offset = 8;
  const imageChunks = [];

  while (offset + 12 <= source.length)
  {
    const length = source.readUInt32BE(offset);
    const type = source.toString('ascii', offset + 4, offset + 8);
    const dataStart = offset + 8;
    const dataEnd = dataStart + length;

    if (dataEnd + 4 > source.length)
    {
      throw new Error('PNG chunk exceeds its input');
    }

    if (type === 'IHDR')
    {
      width = source.readUInt32BE(dataStart);
      height = source.readUInt32BE(dataStart + 4);
      bitDepth = source[dataStart + 8];
      colorType = source[dataStart + 9];
      interlace = source[dataStart + 12];
    }
    else if (type === 'IDAT')
    {
      imageChunks.push(source.subarray(dataStart, dataEnd));
    }
    else if (type === 'IEND')
    {
      break;
    }

    offset = dataEnd + 4;
  }

  if (
    width !== expectedWidth ||
    height !== expectedHeight ||
    bitDepth !== 8 ||
    colorType !== 6 ||
    interlace !== 0
  )
  {
    throw new Error(
      `Expected non-interlaced RGBA8 ${expectedWidth}x${expectedHeight} PNG`,
    );
  }

  const channels = 4;
  const stride = width * channels;
  const filtered = inflateSync(Buffer.concat(imageChunks));

  if (filtered.length !== (stride + 1) * height)
  {
    throw new Error('PNG scanline length does not match IHDR');
  }

  const rgba = Buffer.alloc(width * height * channels);
  let sourceOffset = 0;

  for (let y = 0; y < height; y++)
  {
    const filter = filtered[sourceOffset++];

    for (let x = 0; x < stride; x++)
    {
      const raw = filtered[sourceOffset++];
      const targetOffset = y * stride + x;
      const left = x >= channels ? rgba[targetOffset - channels] : 0;
      const above = y > 0 ? rgba[targetOffset - stride] : 0;
      const upperLeft = y > 0 && x >= channels
        ? rgba[targetOffset - stride - channels]
        : 0;
      let predictor = 0;

      if (filter === 1)
      {
        predictor = left;
      }
      else if (filter === 2)
      {
        predictor = above;
      }
      else if (filter === 3)
      {
        predictor = Math.floor((left + above) * 0.5);
      }
      else if (filter === 4)
      {
        predictor = paeth(left, above, upperLeft);
      }
      else if (filter !== 0)
      {
        throw new Error(`Unsupported PNG scanline filter: ${filter}`);
      }

      rgba[targetOffset] = (raw + predictor) & 0xff;
    }
  }

  return rgba;
}

function appendExtendedLength(output, length)
{
  let remaining = length;

  while (remaining >= 255)
  {
    output.push(255);
    remaining -= 255;
  }
  output.push(remaining);
}

function readSequenceKey(source, offset)
{
  return (
    source[offset] |
    source[offset + 1] << 8 |
    source[offset + 2] << 16 |
    source[offset + 3] << 24
  ) >>> 0;
}

// This deterministic block encoder mirrors the browser implementation. The
// runtime only needs the much smaller, allocation-free LZ4 block decoder.
function encodeLz4Block(source)
{
  const output = [];
  const latestSequence = new Map();
  let anchor = 0;
  let offset = 0;

  while (offset + 4 <= source.length)
  {
    const key = readSequenceKey(source, offset);
    let matchOffset = latestSequence.get(key);

    latestSequence.set(key, offset);
    if (matchOffset === undefined || offset - matchOffset > 0xffff)
    {
      offset++;
      continue;
    }

    while (
      offset > anchor &&
      matchOffset > 0 &&
      source[offset - 1] === source[matchOffset - 1]
    )
    {
      offset--;
      matchOffset--;
    }

    let matchLength = 4;

    while (
      offset + matchLength < source.length &&
      source[offset + matchLength] === source[matchOffset + matchLength]
    )
    {
      matchLength++;
    }

    const literalLength = offset - anchor;
    const encodedMatchLength = matchLength - 4;

    output.push(
      Math.min(literalLength, 15) << 4 |
      Math.min(encodedMatchLength, 15),
    );
    if (literalLength >= 15)
    {
      appendExtendedLength(output, literalLength - 15);
    }
    for (let index = anchor; index < offset; index++)
    {
      output.push(source[index]);
    }

    const distance = offset - matchOffset;

    output.push(distance & 0xff, distance >> 8);
    if (encodedMatchLength >= 15)
    {
      appendExtendedLength(output, encodedMatchLength - 15);
    }

    const matchStart = offset;

    offset += matchLength;
    anchor = offset;
    for (let index = matchStart + 1; index + 4 <= offset; index++)
    {
      latestSequence.set(readSequenceKey(source, index), index);
    }
  }

  const literalLength = source.length - anchor;

  output.push(Math.min(literalLength, 15) << 4);
  if (literalLength >= 15)
  {
    appendExtendedLength(output, literalLength - 15);
  }
  for (let index = anchor; index < source.length; index++)
  {
    output.push(source[index]);
  }

  return Buffer.from(output);
}

function extractRingAlpha(rgba)
{
  const alpha = Buffer.alloc(rgba.length / 4);

  for (let pixel = 0; pixel < alpha.length; pixel++)
  {
    const offset = pixel * 4;

    if (
      rgba[offset] !== 255 ||
      rgba[offset + 1] !== 255 ||
      rgba[offset + 2] !== 255
    )
    {
      throw new Error('Dissolve Ring RGB must remain opaque white');
    }
    alpha[pixel] = rgba[offset + 3];
  }

  return alpha;
}

function addTrailCoverage(rgba, width, height)
{
  const output = Buffer.from(rgba);

  for (let x = 0; x < width; x++)
  {
    let columnPeak = 0;

    for (let y = 0; y < height; y++)
    {
      const offset = (y * width + x) * 4;

      if (rgba[offset + 3] !== 255)
      {
        throw new Error('Trail source Alpha must remain opaque');
      }
      columnPeak = Math.max(
        columnPeak,
        rgba[offset],
        rgba[offset + 1],
        rgba[offset + 2],
      );
    }

    for (let y = 0; y < height; y++)
    {
      const offset = (y * width + x) * 4;
      const support = Math.max(
        rgba[offset],
        rgba[offset + 1],
        rgba[offset + 2],
      );

      output[offset + 3] = columnPeak === 0
        ? 0
        : Math.floor(support / columnPeak * 255 + 0.5);
    }
  }

  return output;
}

function formatByteString(bytes)
{
  const lines = [];
  const bytesPerLine = 32;

  for (let offset = 0; offset < bytes.length; offset += bytesPerLine)
  {
    const end = Math.min(bytes.length, offset + bytesPerLine);
    let line = '    "';

    for (let index = offset; index < end; index++)
    {
      line += `\\x${bytes[index].toString(16).padStart(2, '0').toUpperCase()}`;
    }
    lines.push(`${line}"`);
  }

  return lines.join('\n');
}

function createGeneratedInclude(entries)
{
  const sections = [
    '// Generated by tools/generate-packed-fx-textures.mjs. Do not edit by hand.',
    '// LZ4 blocks are emitted as byte strings so startup skips Base64 and PNG decoding.',
    '',
  ];

  for (const entry of entries)
  {
    sections.push(`inline constexpr char ${entry.id}Lz4Data[] =`);
    sections.push(formatByteString(entry.packed));
    sections.push('    ;');
    sections.push(
      `inline constexpr std::size_t ${entry.id}Lz4ByteCount = ` +
        `sizeof(${entry.id}Lz4Data) - 1U;`,
    );
    sections.push(
      `inline constexpr std::size_t ${entry.id}DecodedByteCount = ` +
        `${entry.decoded.length}U;`,
    );
    sections.push(
      `inline constexpr std::string_view ${entry.id}DecodedSha256 = ` +
        `"${sha256(entry.decoded).toUpperCase()}";`,
    );
    sections.push('');
  }

  return `${sections.join('\n')}\n`;
}

const { outputPath, projectRoot } = parseArguments(process.argv.slice(2));
const textureRoot = join(projectRoot, TEXTURE_DIRECTORY);
const entries = [];

for (const texture of SOURCE_TEXTURES)
{
  const sourcePath = join(textureRoot, texture.file);
  const source = readFileSync(sourcePath);
  const actualSourceHash = sha256(source);

  if (actualSourceHash !== texture.sha256)
  {
    throw new Error(
      `${texture.file} SHA-256 mismatch: ${actualSourceHash}`,
    );
  }

  const rgba = decodePngRgba(source, texture.width, texture.height);
  let decoded = rgba;

  if (texture.id === 'dissolveRing')
  {
    decoded = extractRingAlpha(rgba);
  }
  else if (texture.id === 'trail')
  {
    decoded = addTrailCoverage(rgba, texture.width, texture.height);
  }

  const packed = encodeLz4Block(decoded);

  entries.push({ ...texture, decoded, packed });
  console.log(
    `${texture.id}: ${decoded.length} -> ${packed.length} bytes, ` +
      `SHA-256 ${sha256(decoded)}`,
  );
}

mkdirSync(dirname(outputPath), { recursive: true });
writeFileSync(outputPath, createGeneratedInclude(entries), 'utf8');
console.log(`Generated ${outputPath}`);
