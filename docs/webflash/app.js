const RELEASE_INDEX = "releases/index.json";
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

function absoluteUrl(value) {
  if (!value) {
    return "";
  }
  return new URL(value, window.location.href).href;
}

function toViewModel(release) {
  if (!release.tag || !release.manifestUrl) {
    return null;
  }
  return {
    tag: release.tag,
    prerelease: Boolean(release.prerelease),
    publishedAt: release.publishedAt || "",
    htmlUrl: absoluteUrl(release.htmlUrl),
    commit: release.commit || "",
    manifestUrl: absoluteUrl(release.manifestUrl),
    zipUrl: absoluteUrl(release.zipUrl),
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
  setStatus("Checking WebFlash releases automatically...");
  elements.refreshButton.disabled = true;
  elements.versionSelect.disabled = true;
  elements.installerPanel.hidden = true;

  try {
    const response = await fetch(RELEASE_INDEX, { cache: "no-store" });
    if (!response.ok) {
      throw new Error(`Release index returned ${response.status}`);
    }

    const payload = await response.json();
    if (!Array.isArray(payload.releases)) {
      throw new Error("Release index is invalid.");
    }
    const releases = sortReleases(
      payload.releases.map(toViewModel).filter(Boolean),
    );
    state.releases = releases;
    renderVersionOptions();

    const selected = chooseDefaultRelease(releases);
    if (!selected) {
      setStatus("No WebFlash release was found.", true);
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
    setStatus(error.message || "Failed to read WebFlash releases.", true);
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
loadReleases().catch((error) => setStatus(error.message || "Failed to read WebFlash releases.", true));
