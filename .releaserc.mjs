// semantic-release config. This is JS rather than JSON for exactly one reason:
// the `Changelog:` footer below needs a writer `transform`, and a function
// cannot live in a .json file. Everything else is the same declarative shape it
// was before -- if the footer feature is ever dropped, this can go back to JSON.
import createPreset from 'conventional-changelog-conventionalcommits'

// One copy, used twice: release-notes-generator builds the preset from this, and
// so do we, below, to get at the base transform. Two copies would drift and the
// changelog would silently lose a section.
const presetConfig = {
  types: [
    { type: 'feat', section: 'Features' },
    { type: 'fix', section: 'Fixes' },
    { type: 'compat', section: 'Title compatibility' },
    { type: 'perf', section: 'Performance' },
    { type: 'build', section: 'Build and packaging' },
    { type: 'revert', section: 'Reverts' },
    { type: 'docs', section: 'Documentation', hidden: false },
    { type: 'refactor', section: 'Internal', hidden: false },
    { type: 'test', hidden: true },
    { type: 'ci', hidden: true },
    { type: 'chore', hidden: true },
  ],
}

// --- the `Changelog:` footer --------------------------------------------------
// Commit bodies here are long and internal -- generators, drift guards, parity
// tables -- and none of that belongs in a release note. So the changelog
// description is OPT-IN: write a `Changelog:` footer and its text is rendered
// under the bullet; write nothing and only the subject appears, as before.
//
//     feat(compat-tool): respect Steam's own per-title overlay setting
//
//     <long internal body, not rendered>
//
//     Changelog: Steam's per-game overlay setting now decides whether we
//     inject, so anti-cheat titles can launch.
//
//     Refs #92.
//
// The note runs to the next blank line, so trailing footers (`Refs #92.`) stay
// out of it. No /m flag on the regex: with it, `$` means end-of-LINE and a
// multi-line note silently truncates to its first line.
const CHANGELOG_NOTE = /(?:^|\n)Changelog:[ \t]*([\s\S]*?)(?=\n[ \t]*\n|$)/i

// Two spaces, so the note is a markdown continuation of its list item rather
// than a sibling paragraph that ends the list. Blank lines are left truly empty.
const indent = (text) =>
  text.trim().split("\n").map((l) => (l.trim() ? `  ${l}` : "")).join("\n")

const preset = await createPreset(presetConfig)
const baseTransform = preset.writer.transform

const writerOpts = {
  // The preset's own transform DROPS `body` from what it returns, and the writer
  // then falls back to the raw commit -- which is why this sets `body`
  // explicitly rather than editing what came back. Setting it to null is what
  // keeps an ordinary commit's body out of the changelog.
  transform: (commit, context) => {
    const out = baseTransform(commit, context)
    if (!out) return out
    const source = [commit.body, commit.footer].filter(Boolean).join("\n\n")
    const found = source ? CHANGELOG_NOTE.exec(source) : null
    out.body = found ? indent(found[1]) : null
    return out
  },
  commitPartial: `${preset.writer.commitPartial}{{#if body}}\n\n{{body}}\n{{/if}}`,
}

export default {
  branches: ["main"],
  tagFormat: "v${version}",
  plugins: [
    ["@semantic-release/commit-analyzer", {
      preset: "conventionalcommits",
      releaseRules: [
        { breaking: true, release: "major" },
        { type: "feat", release: "minor" },
        { type: "fix", release: "patch" },
        { type: "compat", release: "minor" },
        { type: "perf", release: "patch" },
        { type: "build", release: "patch" },
        { type: "revert", release: "patch" },
        { type: "docs", release: false },
        { type: "test", release: false },
        { type: "ci", release: false },
        { type: "chore", release: false },
      ],
    }],
    ["@semantic-release/release-notes-generator", {
      preset: "conventionalcommits",
      presetConfig,
      writerOpts,
    }],

    ["@semantic-release/changelog", {
      changelogFile: "CHANGELOG.md",
      changelogTitle: "# Changelog\n\nThis project follows [semantic versioning](https://semver.org).",
    }],

    ["@semantic-release/exec", {
      prepareCmd: "./src/installer/packaging/release-prepare.sh ${nextRelease.version}",
      successCmd: "./src/installer/packaging/push-to-tap.sh ${nextRelease.version}",
    }],

    ["@semantic-release/git", {
      assets: ["VERSION", "CHANGELOG.md"],
      message: "chore(release): ${nextRelease.version}\n\n[skip ci]\n\n${nextRelease.notes}",
    }],

    ["@semantic-release/github", {
      assets: [
        { path: "dist-release/*.tar.gz", label: "Source tarball" },
        { path: "dist-release/*.tar.gz.sha256", label: "Checksum" },
        { path: "dist-release/macos-steam-shim.rb", label: "Homebrew formula" },
      ],
      successComment: false,
      failComment: false,
    }],
  ],
}
