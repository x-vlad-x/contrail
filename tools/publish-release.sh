#!/usr/bin/env bash
# Publishes a release kit made by tools/prepare_release.py on GitHub: a draft prerelease for the existing tag,
# every file listed in SHA256SUMS (and SHA256SUMS itself) uploaded and checked against GitHub's own digests,
# then published. Needs gh (logged in with write access to the repository), python3 and sha256sum.
#
#   tools/publish-release.sh DIRECTORY [TAG]       DIRECTORY holds the release files; TAG defaults to v<version>
#
# Safe to run again: it continues with the draft it made (by release id), replaces nothing that is already
# published, and refuses a kit marked NOT-FOR-RELEASE.
set -euo pipefail
REPO="${CONTRAIL_REPO:-x-vlad-x/contrail}"
DIR="$(cd "${1:?Usage: $0 DIRECTORY [TAG]}" && pwd)"
cd "$DIR"
die() { printf 'Error: %s\n' "$*" >&2; exit 1; }
for tool in gh python3 sha256sum; do command -v "$tool" >/dev/null || die "$tool is required"; done
[[ ! -e NOT-FOR-RELEASE.txt ]] || die "this kit is marked NOT-FOR-RELEASE"
[[ -f SHA256SUMS && -f RELEASE-NOTES.md ]] || die "not a release kit: $DIR"

echo "==> Checking the files"
sha256sum --check --quiet SHA256SUMS
mapfile -t FILES < <(awk '{print $2}' SHA256SUMS)
FILES+=(SHA256SUMS)
extra=$(comm -13 <(printf '%s\n' "${FILES[@]}" | sort) <(ls -1 | sort) || true)
[[ -z $extra ]] || die "files that SHA256SUMS doesn't list: $extra"
VERSION=$(sed -n 's/^# Contrail \(.*\)$/\1/p' RELEASE-NOTES.md | head -1)
TAG="${2:-v$VERSION}"
gh api "repos/$REPO/git/ref/tags/$TAG" >/dev/null 2>&1 || die "tag $TAG doesn't exist on GitHub"

echo "==> Draft release for $TAG"
# Notes without the internal HTML comments.
NOTES=$(python3 -c 'import re, sys; print(re.sub(r"<!--.*?-->\n?", "", open("RELEASE-NOTES.md").read(), flags=re.S))')
ID=$(gh api "repos/$REPO/releases?per_page=100" --jq ".[] | select(.tag_name == \"$TAG\") | .id" | head -1)
if [[ -n $ID ]]; then
  [[ $(gh api "repos/$REPO/releases/$ID" --jq .draft) == true ]] || die "$TAG is already published"
  echo "    continuing with draft $ID"
else
  ID=$(gh api -X POST "repos/$REPO/releases" -f tag_name="$TAG" -f name="Contrail $VERSION" \
         -f body="$NOTES" -F draft=true -F prerelease=true --jq .id)
  echo "    created draft $ID"
fi

echo "==> Uploading ${#FILES[@]} files"
assets() { gh api --paginate "repos/$REPO/releases/$ID/assets?per_page=100" \
             --jq '.[] | [.id, .name, .size, (.digest // ""), .state] | @tsv'; }
uploaded=$(assets)
for name in "${FILES[@]}"; do
  line=$(awk -F'\t' -v n="$name" '$2 == n' <<< "$uploaded")
  if [[ -n $line ]]; then
    IFS=$'\t' read -r asset _ size digest state <<< "$line"
    if [[ $state == uploaded && $size == $(stat -c %s "$name") && $digest == "sha256:$(sha256sum "$name" | cut -d' ' -f1)" ]]; then
      continue
    fi
    gh api -X DELETE "repos/$REPO/releases/assets/$asset" >/dev/null  # an interrupted or different upload
  fi
  echo "    $name"
  gh api -X POST "https://uploads.github.com/repos/$REPO/releases/$ID/assets?name=$name" \
    -H "Content-Type: application/octet-stream" --input "$name" >/dev/null
done

echo "==> Comparing with GitHub's digests"
uploaded=$(assets)
for name in "${FILES[@]}"; do
  IFS=$'\t' read -r _ _ size digest state <<< "$(awk -F'\t' -v n="$name" '$2 == n' <<< "$uploaded")"
  [[ $state == uploaded && $size == $(stat -c %s "$name") ]] || die "upload incomplete: $name"
  [[ -z $digest || $digest == "sha256:$(sha256sum "$name" | cut -d' ' -f1)" ]] || die "digest differs: $name"
done
count=$(wc -l <<< "$uploaded")
[[ $count -eq ${#FILES[@]} ]] || die "the draft has $count files, expected ${#FILES[@]}"

echo "==> Publishing"
gh api -X PATCH "repos/$REPO/releases/$ID" -F draft=false -F prerelease=true >/dev/null
URL=$(gh api "repos/$REPO/releases/$ID" --jq .html_url)
curl -fsSL "https://github.com/$REPO/releases/download/$TAG/SHA256SUMS" | cmp -s - SHA256SUMS \
  || die "the published SHA256SUMS doesn't download correctly"
echo "Published: $URL"
