const ASSET_MANIFEST = "webflash-manifest.json";
const ASSET_ZIP_SUFFIX = ".zip";
const REPOSITORY_OWNER = "Kare-Udon";
const REPOSITORY_NAME = "cardputer-adv-lofi-player";

const state = {
  releases: [],
  selectedTag: "",
};

const elements = {
  repoName: document.querySelector("#repo-name"),
  refreshButton: document.querySelector("#refresh-button"),
  versionSelect: document.querySelector("#version-select"),
  status: document.querySelector("#status"),
  installerPanel: document.querySelector("#installer-panel"),
  installButton: document.querySelector("#install-button"),
  channel: document.querySelector("#channel-value"),
  published: document.querySelector("#published-value"),
  commit: document.querySelector("#commit-value"),
  releaseLink: document.querySelector("#release-link"),
  zipLink: document.querySelector("#zip-link"),
  manifestLink: document.querySelector("#manifest-link"),
};

function setStatus(message, isError = false) {
  elements.status.textContent = message;
  elements.status.classList.toggle("error", isError);
}

function setLink(link, href) {
  if (!href) {
    link.href = "#";
    link.setAttribute("aria-disabled", "true");
    return;
  }
  link.href = href;
  link.setAttribute("aria-disabled", "false");
}

function assetByName(release, name) {
  return release.assets.find((asset) => asset.name === name);
}

function zipAsset(release) {
  return release.assets.find((asset) => asset.name.endsWith(ASSET_ZIP_SUFFIX));
}

function toViewModel(release) {
  const manifest = assetByName(release, ASSET_MANIFEST);
  if (!manifest) {
    return null;
  }
  return {
    tag: release.tag_name,
    prerelease: release.prerelease,
    publishedAt: release.published_at,
    htmlUrl: release.html_url,
    commit: release.target_commitish || "",
    manifestUrl: manifest.browser_download_url,
    zipUrl: zipAsset(release)?.browser_download_url || "",
  };
}

function sortReleases(releases) {
  return [...releases].sort((left, right) => {
    const leftDate = new Date(left.publishedAt || 0).getTime();
    const rightDate = new Date(right.publishedAt || 0).getTime();
    return rightDate - leftDate;
  });
}

function chooseDefaultRelease(releases) {
  return releases.find((release) => !release.prerelease) || releases[0] || null;
}

function formatDate(value) {
  if (!value) {
    return "-";
  }
  return new Intl.DateTimeFormat("en", {
    dateStyle: "medium",
    timeStyle: "short",
  }).format(new Date(value));
}

function renderVersionOptions() {
  elements.versionSelect.innerHTML = "";
  for (const release of state.releases) {
    const option = document.createElement("option");
    option.value = release.tag;
    option.textContent = `${release.tag} ${release.prerelease ? "beta" : "stable"}`;
    elements.versionSelect.append(option);
  }
  elements.versionSelect.disabled = state.releases.length === 0;
}

function renderSelectedRelease() {
  const release = state.releases.find((item) => item.tag === state.selectedTag);
  if (!release) {
    elements.installerPanel.hidden = true;
    elements.channel.textContent = "-";
    elements.published.textContent = "-";
    elements.commit.textContent = "-";
    setLink(elements.releaseLink, "");
    setLink(elements.zipLink, "");
    setLink(elements.manifestLink, "");
    return;
  }

  elements.versionSelect.value = release.tag;
  elements.channel.textContent = release.prerelease ? "beta" : "stable";
  elements.published.textContent = formatDate(release.publishedAt);
  elements.commit.textContent = release.commit.slice(0, 12) || "-";
  elements.installButton.setAttribute("manifest", release.manifestUrl);
  elements.installerPanel.hidden = false;
  setLink(elements.releaseLink, release.htmlUrl);
  setLink(elements.zipLink, release.zipUrl);
  setLink(elements.manifestLink, release.manifestUrl);
}

async function loadReleases() {
  setStatus("Checking GitHub Releases automatically...");
  elements.refreshButton.disabled = true;
  elements.versionSelect.disabled = true;
  elements.installerPanel.hidden = true;

  try {
    const url = `https://api.github.com/repos/${REPOSITORY_OWNER}/${REPOSITORY_NAME}/releases?per_page=30`;
    const response = await fetch(url, {
      headers: { Accept: "application/vnd.github+json" },
    });
    if (!response.ok) {
      throw new Error(`GitHub Releases API returned ${response.status}`);
    }

    const payload = await response.json();
    const releases = sortReleases(
      payload.filter((release) => !release.draft).map(toViewModel).filter(Boolean),
    );
    state.releases = releases;
    renderVersionOptions();

    const selected = chooseDefaultRelease(releases);
    if (!selected) {
      setStatus("No GitHub Release with webflash-manifest.json was found.", true);
      renderSelectedRelease();
      return;
    }

    state.selectedTag = selected.tag;
    renderSelectedRelease();
    setStatus(`Selected ${selected.tag}. Click the install button and choose the device serial port.`);
  } finally {
    elements.refreshButton.disabled = false;
  }
}

elements.refreshButton.addEventListener("click", () => {
  loadReleases().catch((error) => {
    setStatus(error.message || "Failed to read GitHub Releases.", true);
    state.releases = [];
    renderVersionOptions();
    renderSelectedRelease();
  });
});

elements.versionSelect.addEventListener("change", () => {
  state.selectedTag = elements.versionSelect.value;
  renderSelectedRelease();
  setStatus(`Switched to ${state.selectedTag}.`);
});

elements.repoName.textContent = `${REPOSITORY_OWNER}/${REPOSITORY_NAME}`;
loadReleases().catch((error) => setStatus(error.message || "Failed to read GitHub Releases.", true));
