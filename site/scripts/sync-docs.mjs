// Copy the six public buyer pages into the VitePress source tree.
//
// docs/buyer/ is the single source of truth and is never edited from here; site/docs/ is a
// build artifact and is gitignored. Node builtins only — the site must be buildable from a
// bare `npm ci` of vitepress alone.
//
// Two pages under docs/buyer/ are deliberately NOT published: owner-handoff.md (internal
// hardware/bead handoff) and agents-root.md (a SOURCE, materialised into the Fab package as
// the root AGENTS.md by Tools/fab_package.sh:step 4).

import { mkdir, readFile, writeFile, readdir, rm } from 'node:fs/promises'
import { fileURLToPath } from 'node:url'
import path from 'node:path'

const here = path.dirname(fileURLToPath(import.meta.url))
const siteRoot = path.resolve(here, '..')
const repoRoot = path.resolve(siteRoot, '..')
const srcDir = path.join(repoRoot, 'docs', 'buyer')
const outDir = path.join(siteRoot, 'docs')

// Reading order per README.md "Read these, in this order"; localization.md follows the five
// README pages (it is the page ai-guide.md sends you to before the second language).
const PAGES = [
  'setup.md',
  'gotchas.md',
  'rcss-matrix.md',
  'perf-guide.md',
  'ai-guide.md',
  'localization.md'
]

const GITHUB_BLOB = 'https://github.com/ufna/VaCuus/blob/master'

// A link between the six resolves as a route (VitePress rewrites relative .md targets); a
// link that escapes the set has no route to resolve to, so it is rewritten to the file on
// GitHub. Doing it here rather than in the source keeps docs/buyer/ readable as a package
// that a buyer reads on disk, where the relative path is the correct one.
function rewriteLinks(text, file) {
  const outbound = []
  const rewritten = text.replace(
    /(\]\()(?!https?:|\/\/|\/|#|mailto:)([^)\s]+)(\))/g,
    (whole, open, target, close) => {
      const [rawPath, hash = ''] = splitHash(target)
      if (rawPath === '') return whole // pure anchor, e.g. (#section)
      if (PAGES.includes(rawPath)) return whole // stays a route
      const abs = path.resolve(srcDir, rawPath)
      const rel = path.relative(repoRoot, abs).split(path.sep).join('/')
      outbound.push({ file, target, rel })
      return `${open}${GITHUB_BLOB}/${rel}${hash}${close}`
    }
  )
  return { rewritten, outbound }
}

function splitHash(target) {
  const i = target.indexOf('#')
  return i === -1 ? [target, ''] : [target.slice(0, i), target.slice(i)]
}

// Only ever removes .md files this script owns, so a sibling directory (site/public/shots,
// dropped in by the screenshot pass) can never be caught by a stale-copy sweep.
async function pruneStale() {
  let entries = []
  try {
    entries = await readdir(outDir, { withFileTypes: true })
  } catch {
    return
  }
  for (const e of entries) {
    if (e.isFile() && e.name.endsWith('.md') && !PAGES.includes(e.name)) {
      await rm(path.join(outDir, e.name))
      console.log(`  removed stale ${e.name}`)
    }
  }
}

await mkdir(outDir, { recursive: true })
await pruneStale()

const allOutbound = []
for (const page of PAGES) {
  const text = await readFile(path.join(srcDir, page), 'utf8')
  const { rewritten, outbound } = rewriteLinks(text, page)
  allOutbound.push(...outbound)
  await writeFile(path.join(outDir, page), rewritten)
}

console.log(`sync-docs: ${PAGES.length} pages -> ${path.relative(repoRoot, outDir)}`)
for (const o of allOutbound) {
  console.log(`  rewrote ${o.file}: ${o.target} -> ${GITHUB_BLOB}/${o.rel}`)
}
