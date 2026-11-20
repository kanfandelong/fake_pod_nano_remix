(() => {
  "use strict";

  const tracks = [
    {
      title: "Solar Bloom",
      artist: "Mira Vale",
      album: "Daylight Studies",
      duration: 218,
      cover: "assets/cover-solar.svg",
      accent: "#ff7a67",
      accent2: "#ffc36b",
      rgb: "255, 122, 103",
      quality: "FLAC · 24/96"
    },
    {
      title: "Night Transit",
      artist: "Satellite Club",
      album: "After Hours",
      duration: 247,
      cover: "assets/cover-night.svg",
      accent: "#91a4ff",
      accent2: "#c9b8ff",
      rgb: "145, 164, 255",
      quality: "ALAC · 16/44"
    },
    {
      title: "Paper Moon",
      artist: "Lila Gray",
      album: "Soft Focus",
      duration: 194,
      cover: "assets/cover-paper.svg",
      accent: "#c9e06f",
      accent2: "#8fd3a5",
      rgb: "201, 224, 111",
      quality: "FLAC · 24/48"
    },
    {
      title: "Velvet Static",
      artist: "The Quiet Hours",
      album: "Low Signals",
      duration: 232,
      cover: "assets/cover-velvet.svg",
      accent: "#ff6e9a",
      accent2: "#ffb1c5",
      rgb: "255, 110, 154",
      quality: "AAC · 320k"
    },
    {
      title: "Low Tide",
      artist: "Kaito Mori",
      album: "Blue Hour",
      duration: 205,
      cover: "assets/cover-tide.svg",
      accent: "#64d8cf",
      accent2: "#91e8d8",
      rgb: "100, 216, 207",
      quality: "FLAC · 24/96"
    },
    {
      title: "Golden Hour",
      artist: "Mira Vale",
      album: "Daylight Studies",
      duration: 182,
      cover: "assets/cover-solar.svg",
      accent: "#ff7a67",
      accent2: "#ffc36b",
      rgb: "255, 122, 103",
      quality: "FLAC · 24/96"
    },
    {
      title: "Slow Signal",
      artist: "Satellite Club",
      album: "After Hours",
      duration: 276,
      cover: "assets/cover-night.svg",
      accent: "#91a4ff",
      accent2: "#c9b8ff",
      rgb: "145, 164, 255",
      quality: "ALAC · 16/44"
    },
    {
      title: "Glass Garden",
      artist: "Lila Gray",
      album: "Soft Focus",
      duration: 178,
      cover: "assets/cover-paper.svg",
      accent: "#c9e06f",
      accent2: "#8fd3a5",
      rgb: "201, 224, 111",
      quality: "FLAC · 24/48"
    },
    {
      title: "Static Bloom",
      artist: "The Quiet Hours",
      album: "Low Signals",
      duration: 221,
      cover: "assets/cover-velvet.svg",
      accent: "#ff6e9a",
      accent2: "#ffb1c5",
      rgb: "255, 110, 154",
      quality: "AAC · 320k"
    },
    {
      title: "Salt & Silver",
      artist: "Kaito Mori",
      album: "Blue Hour",
      duration: 249,
      cover: "assets/cover-tide.svg",
      accent: "#64d8cf",
      accent2: "#91e8d8",
      rgb: "100, 216, 207",
      quality: "FLAC · 24/96"
    },
    {
      title: "Sun Room",
      artist: "Mira Vale",
      album: "Live Takes",
      duration: 201,
      cover: "assets/cover-solar.svg",
      accent: "#ff7a67",
      accent2: "#ffc36b",
      rgb: "255, 122, 103",
      quality: "FLAC · 24/96"
    },
    {
      title: "Golden Hour (Live)",
      artist: "Mira Vale",
      album: "Live Takes",
      duration: 222,
      cover: "assets/cover-solar.svg",
      accent: "#ff7a67",
      accent2: "#ffc36b",
      rgb: "255, 122, 103",
      quality: "FLAC · 24/96"
    },
    {
      title: "Harbor Lights",
      artist: "Field Notes",
      album: "Coastline",
      duration: 188,
      cover: "assets/cover-tide.svg",
      accent: "#64d8cf",
      accent2: "#91e8d8",
      rgb: "100, 216, 207",
      quality: "WAV · 24/48"
    },
    {
      title: "Rain on Glass",
      artist: "Field Notes",
      album: "Coastline",
      duration: 241,
      cover: "assets/cover-velvet.svg",
      accent: "#ff6e9a",
      accent2: "#ffb1c5",
      rgb: "255, 110, 154",
      quality: "FLAC · 24/96"
    }
  ];

  const trackPaths = [
    "/sdcard/music/01 Solar Bloom.flac",
    "/sdcard/music/02 Night Transit.flac",
    "/sdcard/music/03 Paper Moon.flac",
    "/sdcard/music/04 Velvet Static.flac",
    "/sdcard/music/05 Low Tide.flac",
    "/sdcard/music/06 Golden Hour.flac",
    "/sdcard/music/07 Slow Signal.flac",
    "/sdcard/music/08 Glass Garden.flac",
    "/sdcard/music/09 Static Bloom.flac",
    "/sdcard/music/10 Salt & Silver.flac",
    "/sdcard/music/Live Takes/01 Sun Room.flac",
    "/sdcard/music/Live Takes/02 Golden Hour (Live).flac",
    "/sdcard/field-recordings/Coastline/01 Harbor Lights.wav",
    "/sdcard/field-recordings/Coastline/02 Rain on Glass.flac"
  ];

  tracks.forEach((track, index) => {
    track.path = trackPaths[index];
    track.file = track.path.slice(track.path.lastIndexOf("/") + 1);
    track.folderPath = track.path.slice(0, track.path.lastIndexOf("/"));
    track.index = index;
  });

  const seededPlayCounts = [18, 15, 12, 10, 8, 6, 5, 4, 3, 2, 0, 0, 0, 0];

  const pageNames = {
    now: "NOW PLAYING",
    library: "LIBRARY",
    queue: "UP NEXT",
    settings: "SETTINGS",
    listening: "LISTENING",
    filemanager: "FILES"
  };

  const reviewNotes = {
    now: "Swipe the artwork to skip. Drag the progress line. Open the menu for the rest of the player.",
    library: "Folder-first library: every folder's direct audio files are one playlist. Nested folders get their own playlists.",
    queue: "The queue follows the folder playlist you started. Nested folders are deliberately not merged into it.",
    settings: "Essential controls only: output, EQ, volume, brightness and device state. Sliders are tuned for a single fingertip.",
    listening: "No RTC required: play counts, accumulated play time and favorites are stored locally. No dates, streak or weekly chart.",
    filemanager: "Browse the SD card, play audio outside the library, or add a folder as a new library source."
  };

  const nav = {
    now: document.querySelector('[data-page="now"]'),
    library: document.querySelector('[data-page="library"]'),
    queue: document.querySelector('[data-page="queue"]'),
    settings: document.querySelector('[data-page="settings"]'),
    listening: document.querySelector('[data-page="listening"]'),
    filemanager: document.querySelector('[data-page="filemanager"]')
  };

  const screen = document.getElementById("screen");
  const pages = Array.from(document.querySelectorAll(".page"));
  const pageLabel = document.getElementById("pageLabel");
  const topAction = document.getElementById("topAction");
  const reviewNote = document.getElementById("reviewNote");

  const cover = document.getElementById("cover");
  const quality = document.getElementById("quality");
  const trackTitle = document.getElementById("trackTitle");
  const trackArtist = document.getElementById("trackArtist");
  const trackAlbum = document.getElementById("trackAlbum");
  const likeBtn = document.getElementById("likeBtn");
  const progress = document.getElementById("progress");
  const progressFill = document.getElementById("progressFill");
  const elapsedEl = document.getElementById("elapsed");
  const durationEl = document.getElementById("duration");
  const playBtn = document.getElementById("playBtn");
  const playIconUse = document.querySelector("#playIcon use");
  const shuffleBtn = document.getElementById("shuffleBtn");
  const prevBtn = document.getElementById("prevBtn");
  const nextBtn = document.getElementById("nextBtn");
  const repeatBtn = document.getElementById("repeatBtn");
  const artworkWrap = document.getElementById("artworkWrap");
  const coverImg = document.getElementById("cover");

  const libraryTitle = document.getElementById("libraryTitle");
  const librarySubtitle = document.getElementById("librarySubtitle");
  const libraryList = document.getElementById("libraryList");
  const libraryBody = document.getElementById("libraryBody");
  const playFolderBtn = document.getElementById("playFolderBtn");
  const miniCover = document.getElementById("miniCover");
  const miniTitle = document.getElementById("miniTitle");
  const miniArtist = document.getElementById("miniArtist");
  const miniFill = document.getElementById("miniFill");
  const miniToggle = document.getElementById("miniToggle");
  const miniIconUse = document.querySelector("#miniIcon use");
  const miniOpen = document.getElementById("miniOpen");
  const listeningBtn = document.getElementById("listeningBtn");

  const fileTitle = document.getElementById("fileTitle");
  const fileSubtitle = document.getElementById("fileSubtitle");
  const fileList = document.getElementById("fileList");
  const fileAddBtn = document.getElementById("fileAddBtn");
  const fileAddLabel = document.getElementById("fileAddLabel");
  const fileAddIconUse = document.querySelector("#fileAddIcon use");

  const totalPlaysEl = document.getElementById("totalPlays");
  const totalTimeEl = document.getElementById("totalTime");
  const favCountEl = document.getElementById("favCount");
  const menuListeningEl = document.getElementById("menuListening");
  const topList = document.getElementById("topList");
  const favoritesWrap = document.getElementById("favoritesWrap");
  const favoritesList = document.getElementById("favoritesList");
  const favoritesEmpty = document.getElementById("favoritesEmpty");
  const statTabs = Array.from(document.querySelectorAll("[data-statview]"));

  const queueList = document.getElementById("queueList");
  const queueSummary = document.getElementById("queueSummary");
  const queueShuffleBtn = document.getElementById("queueShuffleBtn");

  const menuOverlay = document.getElementById("menuOverlay");
  const sheetClose = document.getElementById("sheetClose");
  const menuNow = document.getElementById("menuNow");
  const menuCover = document.getElementById("menuCover");
  const menuTitle = document.getElementById("menuTitle");
  const menuArtist = document.getElementById("menuArtist");

  const toast = document.getElementById("toast");

  const reviewLinks = Array.from(document.querySelectorAll("[data-review-screen]"));
  const stage = document.getElementById("stage");
  const html = document.documentElement;

  const state = {
    index: 0,
    position: 84,
    playing: true,
    shuffle: false,
    repeat: "off",
    liked: new Set([0, 2, 4]),
    playCounts: [...seededPlayCounts],
    screen: "now",
    volume: 62,
    brightness: 78,
    scrubbing: false,
    menuOpen: false,
    libraryRoots: ["/sdcard/music"],
    libraryFolder: null,
    fileFolder: "/sdcard",
    queue: [],
    queueLabel: "Queue"
  };

  let toastTimer = null;
  let statView = "top";
  let artDrag = null;

  const query = new URLSearchParams(window.location.search);
  const captureMode = query.get("capture") === "1";
  if (captureMode) html.classList.add("capture-mode");

  const clamp = (value, min, max) => Math.min(max, Math.max(min, value));
  const esc = (value) => String(value).replace(/[&<>"']/g, (char) => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    '"': "&quot;",
    "'": "&#39;"
  }[char]));

  const current = () => tracks[state.index];

  function formatTime(totalSeconds) {
    const safe = Math.max(0, Math.floor(totalSeconds));
    return `${Math.floor(safe / 60)}:${String(safe % 60).padStart(2, "0")}`;
  }

  const extraFolders = [
    "/sdcard",
    "/sdcard/music",
    "/sdcard/music/Live Takes",
    "/sdcard/field-recordings",
    "/sdcard/field-recordings/Coastline",
    "/sdcard/downloads",
    "/sdcard/audiobooks",
    "/sdcard/podcasts"
  ];
  const folderMap = new Map();

  const plural = (count, word) => `${count} ${word}${count === 1 ? "" : "s"}`;

  function parentPathOf(path) {
    const slash = path.lastIndexOf("/");
    return slash <= 0 ? "/" : path.slice(0, slash);
  }

  function ensureFolder(path) {
    if (!path || path === "/") return null;
    if (folderMap.has(path)) return folderMap.get(path);
    const parent = ensureFolder(parentPathOf(path));
    const folder = {
      path,
      name: path.slice(path.lastIndexOf("/") + 1),
      parentPath: parent ? parent.path : null,
      folders: [],
      tracks: []
    };
    folderMap.set(path, folder);
    if (parent) parent.folders.push(folder);
    return folder;
  }

  function buildFolderTree() {
    tracks.forEach((track, index) => {
      const folder = ensureFolder(track.folderPath);
      if (folder) folder.tracks.push(index);
    });
    extraFolders.forEach(ensureFolder);
    folderMap.forEach((folder) => {
      folder.folders.sort((a, b) => a.name.localeCompare(b.name));
      folder.tracks.sort((a, b) => tracks[a].file.localeCompare(tracks[b].file));
    });
  }

  function collectTrackIndices(folder, set = new Set()) {
    folder.tracks.forEach((index) => set.add(index));
    folder.folders.forEach((child) => collectTrackIndices(child, set));
    return set;
  }

  function playlistCountIn(folder) {
    return (folder.tracks.length > 0 ? 1 : 0)
      + folder.folders.reduce((sum, child) => sum + playlistCountIn(child), 0);
  }

  function shortPath(path) {
    const relative = path.replace(/^\/sdcard\/?/, "");
    return relative ? relative.split("/").join(" / ") : "sdcard";
  }

  function isPathInLibrary(path) {
    return state.libraryRoots.some((root) => path === root || path.startsWith(`${root}/`));
  }

  function libraryRootFolders() {
    return state.libraryRoots.map((path) => folderMap.get(path)).filter(Boolean);
  }

  function libraryTrackSet() {
    const set = new Set();
    libraryRootFolders().forEach((folder) => collectTrackIndices(folder, set));
    return set;
  }

  function librarySourceRow(folder) {
    const trackCount = collectTrackIndices(folder).size;
    const playlists = playlistCountIn(folder);
    const isDefault = folder.path === "/sdcard/music";
    return `
      <button class="source-row" data-library-folder="${esc(folder.path)}" aria-label="Open library source ${esc(folder.name)}">
        <span class="folder-tile source-tile">
          <svg class="icon" aria-hidden="true"><use href="#i-folder"></use></svg>
        </span>
        <span class="source-copy">
          <span class="folder-name"><span class="folder-name-text">${esc(folder.name)}</span>${isDefault ? '<span class="default-badge">DEFAULT</span>' : ""}</span>
          <span class="source-meta">${esc(folder.path)} · ${plural(playlists, "playlist")} · ${plural(trackCount, "track")}</span>
        </span>
        <svg class="icon row-chevron" aria-hidden="true"><use href="#i-chevron"></use></svg>
      </button>
    `;
  }

  function libraryFolderRow(folder, context) {
    const direct = folder.tracks.length;
    const nested = folder.folders.length;
    const metaParts = [];
    if (direct) metaParts.push(plural(direct, "track"));
    if (nested) metaParts.push(plural(nested, "folder"));
    if (!metaParts.length) metaParts.push("Empty folder");
    const inLibrary = isPathInLibrary(folder.path);
    const badge = context === "files" && inLibrary ? '<span class="lib-badge">IN LIBRARY</span>' : "";
    const attribute = context === "files"
      ? `data-file-folder="${esc(folder.path)}"`
      : `data-library-folder="${esc(folder.path)}"`;
    return `
      <button class="folder-row" ${attribute} aria-label="Open folder ${esc(folder.name)}">
        <span class="folder-tile">
          <svg class="icon" aria-hidden="true"><use href="#i-folder"></use></svg>
        </span>
        <span class="folder-copy">
          <span class="folder-name"><span class="folder-name-text">${esc(folder.name)}</span>${badge}</span>
          <span class="folder-meta">${metaParts.join(" · ")}</span>
        </span>
        <svg class="icon row-chevron" aria-hidden="true"><use href="#i-chevron"></use></svg>
      </button>
    `;
  }

  function libraryFileRow(index, context) {
    const track = tracks[index];
    const title = context === "files" ? track.file : track.title;
    return `
      <button class="song-row library-file-row${index === state.index ? " is-current" : ""}" data-index="${index}" aria-label="Play ${esc(title)}">
        <img class="song-cover" src="${track.cover}" alt="" draggable="false">
        <span class="song-copy">
          <span class="song-title">${esc(title)}</span>
          <span class="song-artist">${esc(track.quality)}</span>
        </span>
        <span class="song-time">${formatTime(track.duration)}</span>
      </button>
    `;
  }

  function listLabel(text) {
    return `<div class="list-label">${esc(text)}</div>`;
  }

  function emptyFolder(message) {
    return `<div class="folder-empty"><span class="folder-empty-icon"><svg class="icon" aria-hidden="true"><use href="#i-folder"></use></svg></span><p>${esc(message)}</p></div>`;
  }

  function renderLibrary() {
    let folder = state.libraryFolder ? folderMap.get(state.libraryFolder) : null;
    if (state.libraryFolder && !folder) {
      state.libraryFolder = null;
      folder = null;
    }

    if (!folder) {
      const roots = libraryRootFolders();
      const allTracks = libraryTrackSet();
      const playlists = roots.reduce((sum, root) => sum + playlistCountIn(root), 0);
      libraryTitle.textContent = "Library";
      librarySubtitle.textContent = `${plural(roots.length, "source")} · ${plural(playlists, "playlist")} · ${plural(allTracks.size, "track")}`;
      playFolderBtn.hidden = true;
      libraryList.innerHTML = [
        listLabel("Sources"),
        ...roots.map(librarySourceRow),
        `<button class="add-folder-row" data-action="open-files" aria-label="Add music folder">
          <span class="add-folder-icon"><svg class="icon" aria-hidden="true"><use href="#i-folder-plus"></use></svg></span>
          <span class="add-folder-copy">
            <span class="add-folder-title">Add music folder</span>
            <span class="add-folder-meta">Browse /sdcard or another folder</span>
          </span>
        </button>`,
        `<p class="library-note">Each folder is one playlist. Nested folders stay separate.</p>`
      ].join("");
    } else {
      const direct = folder.tracks.length;
      libraryTitle.textContent = folder.name;
      librarySubtitle.textContent = `${folder.path} · ${plural(direct, "track")} in this folder`;
      playFolderBtn.hidden = direct === 0;

      let html = "";
      if (folder.folders.length) {
        html += listLabel("Folders");
        html += folder.folders.map((child) => libraryFolderRow(child, "library")).join("");
      }
      if (direct) {
        html += listLabel(`Folder playlist · ${plural(direct, "track")}`);
        html += folder.tracks.map((index) => libraryFileRow(index, "library")).join("");
      }
      if (!html) {
        html = emptyFolder("This folder has no direct audio files.");
      }
      libraryList.innerHTML = html;
    }

    const libraryTileSub = document.querySelector('[data-goto="library"] small');
    if (libraryTileSub) libraryTileSub.textContent = plural(libraryTrackSet().size, "track");
  }

  function renderFileManager() {
    let folder = folderMap.get(state.fileFolder);
    if (!folder) {
      state.fileFolder = "/sdcard";
      folder = folderMap.get("/sdcard");
    }

    fileTitle.textContent = folder.path === "/sdcard" ? "Files" : folder.name;
    fileSubtitle.textContent = `${folder.path} · ${plural(folder.tracks.length, "file")}`;

    let html = "";
    if (folder.folders.length) {
      html += listLabel("Folders");
      html += folder.folders.map((child) => libraryFolderRow(child, "files")).join("");
    }
    if (folder.tracks.length) {
      html += listLabel("Audio files");
      html += folder.tracks.map((index) => libraryFileRow(index, "files")).join("");
    }
    if (!html) {
      html = emptyFolder("This folder is empty.");
    }
    fileList.innerHTML = html;

    const isSdRoot = folder.path === "/sdcard";
    const covered = isPathInLibrary(folder.path);
    const hasAudio = collectTrackIndices(folder).size > 0;
    const canAdd = !isSdRoot && !covered && hasAudio;
    fileAddBtn.disabled = !canAdd;
    fileAddBtn.classList.toggle("is-added", covered);
    fileAddLabel.textContent = canAdd
      ? "Add this folder"
      : isSdRoot
        ? "Open a folder to add"
        : covered
          ? "In library"
          : "No audio in this folder";
    fileAddIconUse.setAttribute("href", covered ? "#i-check" : isSdRoot || !hasAudio ? "#i-folder" : "#i-folder-plus");
  }

  function totalQueueMinutes() {
    const seconds = state.queue.reduce((sum, index) => sum + tracks[index].duration, 0);
    return Math.round(seconds / 60);
  }

  function buildQueue() {
    queueSummary.textContent = `${state.queueLabel} · ${plural(state.queue.length, "track")}`;
    queueList.innerHTML = state.queue.map((index, position) => {
      const track = tracks[index];
      const isCurrent = index === state.index;
      return `
        <button class="queue-row${isCurrent ? " is-current" : ""}" data-index="${index}" aria-label="Play ${esc(track.title)} by ${esc(track.artist)}">
          <span class="queue-index">${isCurrent
            ? '<span class="eq" aria-hidden="true"><i></i><i></i><i></i></span>'
            : String(position + 1).padStart(2, "0")}</span>
          <img class="queue-cover" src="${track.cover}" alt="" draggable="false">
          <span class="queue-copy">
            <span class="queue-title">${esc(track.title)}</span>
            <span class="queue-artist">${esc(track.artist)}</span>
          </span>
          <span class="queue-time">${formatTime(track.duration)}</span>
          <span class="queue-grip" aria-hidden="true">
            <svg class="icon"><use href="#i-grip"></use></svg>
          </span>
        </button>
      `;
    }).join("");

    const queueTileSub = document.querySelector('[data-goto="queue"] small');
    if (queueTileSub) queueTileSub.textContent = plural(state.queue.length, "track");
  }

  function formatPlayTime(totalSeconds) {
    const totalMinutes = Math.round(totalSeconds / 60);
    const hours = Math.floor(totalMinutes / 60);
    const minutes = totalMinutes % 60;
    return hours > 0 ? `${hours}h ${String(minutes).padStart(2, "0")}m` : `${minutes}m`;
  }

  function applyStatView() {
    statTabs.forEach((tab) => {
      const active = tab.dataset.statview === statView;
      tab.classList.toggle("is-active", active);
      tab.setAttribute("aria-selected", String(active));
    });
    topList.hidden = statView !== "top";
    favoritesWrap.hidden = statView !== "favorites";
  }

  function renderStats() {
    const favoriteIndices = tracks
      .map((track, index) => index)
      .filter((index) => state.liked.has(index));
    const totalPlays = state.playCounts.reduce((sum, count) => sum + count, 0);
    const totalSeconds = state.playCounts.reduce((sum, count, index) => {
      return sum + count * tracks[index].duration;
    }, 0);

    totalPlaysEl.textContent = String(totalPlays);
    totalTimeEl.textContent = formatPlayTime(totalSeconds);
    favCountEl.textContent = String(favoriteIndices.length);
    if (menuListeningEl) menuListeningEl.textContent = formatPlayTime(totalSeconds);

    const ranked = tracks
      .map((track, index) => ({ track, index, plays: state.playCounts[index] }))
      .filter((entry) => entry.plays > 0)
      .sort((a, b) => b.plays - a.plays || a.index - b.index);
    const maxPlays = Math.max(...state.playCounts, 1);

    topList.innerHTML = ranked.map(({ track, index, plays }, rank) => `
      <button class="stat-row${index === state.index ? " is-current" : ""}" data-index="${index}" aria-label="Play ${esc(track.title)} by ${esc(track.artist)}, ${plays} plays">
        <span class="stat-rank${rank === 0 ? " is-first" : ""}">${String(rank + 1).padStart(2, "0")}</span>
        <img class="stat-cover" src="${track.cover}" alt="" draggable="false">
        <span class="stat-copy">
          <span class="stat-title">${esc(track.title)}</span>
          <span class="stat-artist">${esc(track.artist)}</span>
          <span class="stat-meter" aria-hidden="true"><i style="width:${Math.max(5, (plays / maxPlays) * 100)}%"></i></span>
        </span>
        <span class="stat-count"><strong>${plays}</strong><small>plays</small></span>
      </button>
    `).join("");

    favoritesList.innerHTML = favoriteIndices.map((index) => {
      const track = tracks[index];
      return `
        <div class="stat-row favorite-row${index === state.index ? " is-current" : ""}">
          <button class="favorite-play" data-index="${index}" aria-label="Play favorite ${esc(track.title)} by ${esc(track.artist)}">
            <img class="stat-cover" src="${track.cover}" alt="" draggable="false">
            <span class="stat-copy">
              <span class="stat-title">${esc(track.title)}</span>
              <span class="stat-artist">${esc(track.artist)} · ${esc(track.album)}</span>
            </span>
          </button>
          <button class="stat-heart" data-unfav="${index}" aria-label="Remove ${esc(track.title)} from favorites">
            <svg class="icon"><use href="#i-heart"></use></svg>
          </button>
        </div>
      `;
    }).join("");

    const hasFavorites = favoriteIndices.length > 0;
    favoritesList.hidden = !hasFavorites;
    favoritesEmpty.hidden = hasFavorites;
    applyStatView();
  }

  function setStatView(view) {
    if (!["top", "favorites"].includes(view)) return;
    statView = view;
    applyStatView();
    const body = document.querySelector(".stats-body");
    if (body) body.scrollTop = 0;
  }

  function updateCurrentRows() {
    [libraryList, fileList].forEach((list) => {
      list.querySelectorAll(".library-file-row[data-index]").forEach((row) => {
        const isCurrent = Number(row.dataset.index) === state.index;
        row.classList.toggle("is-current", isCurrent);
        if (isCurrent) {
          row.setAttribute("aria-current", "true");
        } else {
          row.removeAttribute("aria-current");
        }
      });
    });
  }

  function updateLike() {
    const liked = state.liked.has(state.index);
    likeBtn.classList.toggle("is-liked", liked);
    likeBtn.setAttribute("aria-pressed", String(liked));
    likeBtn.setAttribute("aria-label", liked ? "Remove from favorites" : "Add to favorites");
  }

  function animateCover() {
    coverImg.classList.remove("cover-enter");
    void coverImg.offsetWidth;
    coverImg.classList.add("cover-enter");
  }

  function applyTrack(index, options = {}) {
    const count = tracks.length;
    state.index = ((index % count) + count) % count;
    const track = current();

    screen.style.setProperty("--accent", track.accent);
    screen.style.setProperty("--accent-2", track.accent2);
    screen.style.setProperty("--accent-rgb", track.rgb);
    html.style.setProperty("--page-accent", track.accent);
    html.style.setProperty("--accent-rgb", track.rgb);

    cover.src = track.cover;
    cover.alt = `${track.title} album artwork`;
    coverImg.alt = cover.alt;
    trackTitle.textContent = track.title;
    trackTitle.title = track.title;
    trackArtist.textContent = track.artist;
    trackAlbum.textContent = track.album;
    quality.textContent = track.quality;
    durationEl.textContent = formatTime(track.duration);

    miniCover.src = track.cover;
    miniTitle.textContent = track.title;
    miniArtist.textContent = track.artist;
    menuCover.src = track.cover;
    menuTitle.textContent = track.title;
    menuArtist.textContent = track.artist;

    updateCurrentRows();
    updateLike();
    buildQueue();
    renderStats();
    updateProgress();
    if (options.animate !== false) animateCover();
    document.title = `${track.title} — ${track.artist}`;
  }

  function updatePlayIcons() {
    const icon = state.playing ? "#i-pause" : "#i-play";
    playIconUse.setAttribute("href", icon);
    miniIconUse.setAttribute("href", icon);
    playBtn.setAttribute("aria-label", state.playing ? "Pause" : "Play");
    miniToggle.setAttribute("aria-label", state.playing ? "Pause" : "Play");
    playBtn.classList.toggle("is-playing", state.playing);
    screen.classList.toggle("is-paused", !state.playing);
  }

  function setPlaying(playing) {
    state.playing = Boolean(playing);
    updatePlayIcons();
  }

  function togglePlay() {
    setPlaying(!state.playing);
    showToast(state.playing ? "Playing" : "Paused");
  }

  function updateProgress() {
    const track = current();
    const fraction = track.duration ? clamp(state.position / track.duration, 0, 1) : 0;
    const percent = fraction * 100;
    progressFill.style.width = `${percent}%`;
    elapsedEl.textContent = formatTime(state.position);
    durationEl.textContent = formatTime(track.duration);
    progress.setAttribute("aria-valuenow", String(Math.round(percent)));
    progress.setAttribute("aria-valuetext", `${formatTime(state.position)} of ${formatTime(track.duration)}`);
    miniFill.style.width = `${percent}%`;
  }

  function useFolderQueue(index) {
    const track = tracks[index];
    const folder = folderMap.get(track.folderPath);
    state.queue = folder ? folder.tracks.slice() : [index];
    state.queueLabel = folder ? folder.name : "Queue";
  }

  function playFromFolder(index) {
    useFolderQueue(index);
    playTrack(index, { play: true });
  }

  function playTrack(index, options = {}) {
    const count = tracks.length;
    const nextIndex = ((index % count) + count) % count;
    state.playCounts[nextIndex] += 1;
    state.position = 0;
    if (options.play !== false) {
      state.playing = true;
      updatePlayIcons();
    }
    applyTrack(nextIndex, { animate: true });
    setPlaying(state.playing);
  }

  function nextTrack(options = {}) {
    const queue = state.queue.length ? state.queue : tracks.map((track, index) => index);
    const position = Math.max(0, queue.indexOf(state.index));
    let nextIndex;

    if (state.shuffle && queue.length > 1) {
      do {
        nextIndex = queue[Math.floor(Math.random() * queue.length)];
      } while (nextIndex === state.index);
    } else {
      const nextPosition = position + 1;
      if (nextPosition >= queue.length) {
        if (state.repeat === "off" && options.auto) {
          state.position = current().duration;
          setPlaying(false);
          updateProgress();
          return;
        }
        nextIndex = queue[0];
      } else {
        nextIndex = queue[nextPosition];
      }
    }
    playTrack(nextIndex, { play: true });
  }

  function previousTrack() {
    if (state.position > 3) {
      state.position = 0;
      updateProgress();
      return;
    }
    const queue = state.queue.length ? state.queue : tracks.map((track, index) => index);
    const position = queue.indexOf(state.index);
    const previousPosition = position <= 0 ? queue.length - 1 : position - 1;
    playTrack(queue[previousPosition], { play: true });
  }

  function setShuffle(enabled, announce = true) {
    state.shuffle = Boolean(enabled);
    [shuffleBtn, queueShuffleBtn].forEach((button) => {
      button.classList.toggle("is-active", state.shuffle);
      button.setAttribute("aria-pressed", String(state.shuffle));
    });
    shuffleBtn.setAttribute("aria-label", state.shuffle ? "Shuffle on" : "Shuffle off");
    if (announce) showToast(state.shuffle ? "Shuffle on" : "Shuffle off");
  }

  function setRepeat(mode, announce = true) {
    state.repeat = mode;
    const active = mode !== "off";
    repeatBtn.classList.toggle("is-active", active);
    repeatBtn.classList.toggle("is-one", mode === "one");
    repeatBtn.setAttribute("aria-pressed", String(active));
    repeatBtn.setAttribute("aria-label", mode === "off" ? "Repeat off" : mode === "all" ? "Repeat all" : "Repeat one");
    if (announce) {
      showToast(mode === "off" ? "Repeat off" : mode === "all" ? "Repeat all" : "Repeat one");
    }
  }

  function seekFromClientX(clientX) {
    const rect = progress.getBoundingClientRect();
    const ratio = clamp((clientX - rect.left) / rect.width, 0, 1);
    state.position = ratio * current().duration;
    updateProgress();
  }

  function showToast(message) {
    if (!toast) return;
    toast.textContent = message;
    toast.classList.add("is-visible");
    window.clearTimeout(toastTimer);
    toastTimer = window.setTimeout(() => {
      toast.classList.remove("is-visible");
    }, 1600);
  }

  function syncReviewLinks() {
    reviewLinks.forEach((link) => {
      link.classList.toggle("is-active", link.dataset.reviewScreen === state.screen);
      if (link.dataset.reviewScreen === state.screen) {
        link.setAttribute("aria-current", "page");
      } else {
        link.removeAttribute("aria-current");
      }
    });
    if (reviewNote) reviewNote.textContent = reviewNotes[state.screen] || "";
  }

  function updateTopAction() {
    let label = "Back to now playing";
    if (state.screen === "now") {
      label = "Open menu";
    } else if (state.screen === "library") {
      label = state.libraryFolder ? "Up one folder" : "Back to now playing";
    } else if (state.screen === "filemanager") {
      label = state.fileFolder === "/sdcard" ? "Back to library" : "Up one folder";
    }
    topAction.setAttribute("aria-label", label);
  }

  function navigate(name, options = {}) {
    if (!nav[name]) return;
    state.screen = name;
    screen.dataset.screen = name;
    pageLabel.textContent = pageNames[name];

    pages.forEach((page) => {
      const active = page.dataset.page === name;
      page.classList.toggle("is-active", active);
      page.setAttribute("aria-hidden", String(!active));
      if (active) {
        page.removeAttribute("inert");
      } else {
        page.setAttribute("inert", "");
      }
    });

    updateTopAction();
    closeMenu(false);
    syncReviewLinks();

    if (name === "listening") renderStats();
    if (name === "library") renderLibrary();
    if (name === "filemanager") renderFileManager();
    updateCurrentRows();

    if (name === "queue") {
      requestAnimationFrame(() => {
        const currentRow = queueList.querySelector(".queue-row.is-current");
        if (currentRow) currentRow.scrollIntoView({ block: "center" });
      });
    }

    if (options.focus) {
      const first = nav[name].querySelector("button");
      if (first) first.focus({ preventScroll: true });
    }
  }

  function openMenu() {
    if (state.menuOpen) return;
    state.menuOpen = true;
    menuOverlay.classList.add("is-open");
    menuOverlay.setAttribute("aria-hidden", "false");
    sheetClose.focus({ preventScroll: true });
  }

  function closeMenu(returnFocus = true) {
    if (!state.menuOpen) return;
    state.menuOpen = false;
    menuOverlay.classList.remove("is-open");
    menuOverlay.setAttribute("aria-hidden", "true");
    if (returnFocus && state.screen === "now") {
      topAction.focus({ preventScroll: true });
    }
  }

  function openFileManager() {
    state.fileFolder = "/sdcard";
    navigate("filemanager");
  }

  function libraryBack() {
    if (!state.libraryFolder) {
      navigate("now");
      return;
    }
    const folder = folderMap.get(state.libraryFolder);
    if (!folder || state.libraryRoots.includes(folder.path) || !folder.parentPath || folder.parentPath === "/sdcard") {
      state.libraryFolder = null;
    } else {
      state.libraryFolder = folder.parentPath;
    }
    renderLibrary();
    updateTopAction();
    libraryBody.scrollTop = 0;
  }

  function fileManagerBack() {
    if (state.fileFolder === "/sdcard") {
      navigate("library");
      return;
    }
    const parent = parentPathOf(state.fileFolder);
    state.fileFolder = parent && parent !== "/" ? parent : "/sdcard";
    renderFileManager();
    updateTopAction();
  }

  function handleBack() {
    if (state.menuOpen) {
      closeMenu();
      return;
    }
    if (state.screen === "library") {
      libraryBack();
      return;
    }
    if (state.screen === "filemanager") {
      fileManagerBack();
      return;
    }
    if (state.screen !== "now") {
      navigate("now");
    }
  }

  function updateRange(input) {
    const min = Number(input.min || 0);
    const max = Number(input.max || 100);
    const value = Number(input.value);
    const percent = ((value - min) / (max - min)) * 100;
    const range = input.closest(".range");
    if (range) range.style.setProperty("--p", `${percent}%`);
  }

  function syncVolume(value, source) {
    state.volume = clamp(Math.round(value), 0, 100);
    document.querySelectorAll("[data-volume-range]").forEach((input) => {
      if (input !== source) input.value = String(state.volume);
      updateRange(input);
    });
    document.querySelectorAll("[data-volume-value]").forEach((element) => {
      if (element.classList.contains("menu-volume-value")) {
        element.textContent = String(state.volume);
      } else {
        element.textContent = `${state.volume}%`;
      }
    });
  }

  function fitStage() {
    if (captureMode) return;
    const width = window.innerWidth;
    const height = window.innerHeight;
    let scale;
    if (width <= 1080) {
      scale = clamp((width - 40) / 480, 0.58, 1.1);
    } else {
      scale = clamp((height - 185) / 480, 0.8, 1.18);
    }
    html.style.setProperty("--scale", String(scale));
  }

  function handleEscape() {
    handleBack();
  }

  /* ---------------- events ---------------- */

  topAction.addEventListener("click", () => {
    if (state.screen === "now") {
      if (state.menuOpen) closeMenu();
      else openMenu();
    } else {
      handleBack();
    }
  });

  playBtn.addEventListener("click", togglePlay);
  miniToggle.addEventListener("click", togglePlay);
  prevBtn.addEventListener("click", previousTrack);
  nextBtn.addEventListener("click", () => nextTrack());
  shuffleBtn.addEventListener("click", () => setShuffle(!state.shuffle));
  queueShuffleBtn.addEventListener("click", () => setShuffle(!state.shuffle));
  repeatBtn.addEventListener("click", () => {
    const modes = ["off", "all", "one"];
    setRepeat(modes[(modes.indexOf(state.repeat) + 1) % modes.length]);
  });

  likeBtn.addEventListener("click", () => {
    if (state.liked.has(state.index)) {
      state.liked.delete(state.index);
      showToast("Removed from favorites");
    } else {
      state.liked.add(state.index);
      showToast("Added to favorites");
    }
    updateLike();
    renderStats();
  });

  miniOpen.addEventListener("click", () => navigate("now"));
  listeningBtn.addEventListener("click", () => navigate("listening"));

  playFolderBtn.addEventListener("click", () => {
    const folder = state.libraryFolder ? folderMap.get(state.libraryFolder) : null;
    if (!folder || !folder.tracks.length) return;
    state.queue = folder.tracks.slice();
    state.queueLabel = folder.name;
    playTrack(folder.tracks[0], { play: true });
    navigate("now");
  });

  libraryList.addEventListener("click", (event) => {
    const add = event.target.closest('[data-action="open-files"]');
    if (add) {
      openFileManager();
      return;
    }
    const folderRow = event.target.closest("[data-library-folder]");
    if (folderRow) {
      state.libraryFolder = folderRow.dataset.libraryFolder;
      renderLibrary();
      updateTopAction();
      libraryBody.scrollTop = 0;
      return;
    }
    const fileRow = event.target.closest(".library-file-row[data-index]");
    if (fileRow) {
      playFromFolder(Number(fileRow.dataset.index));
      navigate("now");
    }
  });

  fileList.addEventListener("click", (event) => {
    const folderRow = event.target.closest("[data-file-folder]");
    if (folderRow) {
      state.fileFolder = folderRow.dataset.fileFolder;
      renderFileManager();
      updateTopAction();
      fileList.scrollTop = 0;
      return;
    }
    const fileRow = event.target.closest(".library-file-row[data-index]");
    if (fileRow) {
      playFromFolder(Number(fileRow.dataset.index));
      navigate("now");
    }
  });

  fileAddBtn.addEventListener("click", () => {
    if (fileAddBtn.disabled || state.fileFolder === "/sdcard" || isPathInLibrary(state.fileFolder)) return;
    if (!state.libraryRoots.includes(state.fileFolder)) {
      state.libraryRoots.push(state.fileFolder);
    }
    showToast("Added to library");
    renderFileManager();
    renderLibrary();
  });

  statTabs.forEach((tab) => {
    tab.addEventListener("click", () => setStatView(tab.dataset.statview));
  });

  topList.addEventListener("click", (event) => {
    const row = event.target.closest(".stat-row");
    if (!row) return;
    playFromFolder(Number(row.dataset.index));
    navigate("now");
  });

  favoritesList.addEventListener("click", (event) => {
    const remove = event.target.closest("[data-unfav]");
    if (remove) {
      const index = Number(remove.dataset.unfav);
      state.liked.delete(index);
      updateLike();
      renderStats();
      showToast("Removed from favorites");
      return;
    }
    const play = event.target.closest("[data-index]");
    if (!play) return;
    playFromFolder(Number(play.dataset.index));
    navigate("now");
  });

  queueList.addEventListener("click", (event) => {
    const row = event.target.closest(".queue-row");
    if (!row) return;
    playTrack(Number(row.dataset.index), { play: true });
    navigate("now");
  });

  reviewLinks.forEach((link) => {
    link.addEventListener("click", () => navigate(link.dataset.reviewScreen));
  });

  menuOverlay.addEventListener("click", (event) => {
    if (event.target === menuOverlay) {
      closeMenu();
      return;
    }
    const tile = event.target.closest("[data-goto]");
    if (tile) navigate(tile.dataset.goto);
  });

  menuNow.addEventListener("click", () => {
    closeMenu(false);
    navigate("now");
  });

  sheetClose.addEventListener("click", () => closeMenu());

  /* progress scrubbing */
  progress.addEventListener("pointerdown", (event) => {
    if (event.pointerType === "mouse" && event.button !== 0) return;
    state.scrubbing = true;
    progress.classList.add("is-scrubbing");
    progress.setPointerCapture(event.pointerId);
    seekFromClientX(event.clientX);
    event.preventDefault();
  });

  progress.addEventListener("pointermove", (event) => {
    if (!state.scrubbing) return;
    seekFromClientX(event.clientX);
  });

  const endScrub = (event) => {
    if (!state.scrubbing) return;
    state.scrubbing = false;
    progress.classList.remove("is-scrubbing");
    if (progress.hasPointerCapture(event.pointerId)) {
      progress.releasePointerCapture(event.pointerId);
    }
  };
  progress.addEventListener("pointerup", endScrub);
  progress.addEventListener("pointercancel", endScrub);

  progress.addEventListener("keydown", (event) => {
    const step = event.shiftKey ? 15 : 5;
    if (event.key === "ArrowLeft") {
      state.position = clamp(state.position - step, 0, current().duration);
      updateProgress();
      event.preventDefault();
    } else if (event.key === "ArrowRight") {
      state.position = clamp(state.position + step, 0, current().duration);
      updateProgress();
      event.preventDefault();
    } else if (event.key === "Home") {
      state.position = 0;
      updateProgress();
      event.preventDefault();
    } else if (event.key === "End") {
      state.position = current().duration;
      updateProgress();
      event.preventDefault();
    }
  });

  /* artwork swipe */
  artworkWrap.addEventListener("pointerdown", (event) => {
    if (event.pointerType === "mouse" && event.button !== 0) return;
    artDrag = {
      id: event.pointerId,
      startX: event.clientX,
      startY: event.clientY,
      dx: 0,
      active: false
    };
    artworkWrap.setPointerCapture(event.pointerId);
  });

  artworkWrap.addEventListener("pointermove", (event) => {
    if (!artDrag || event.pointerId !== artDrag.id) return;
    const dx = event.clientX - artDrag.startX;
    const dy = event.clientY - artDrag.startY;

    if (!artDrag.active && Math.abs(dx) > 8 && Math.abs(dx) > Math.abs(dy) * 1.2) {
      artDrag.active = true;
      artworkWrap.classList.add("is-dragging");
    }

    if (!artDrag.active) return;
    artDrag.dx = dx;
    artworkWrap.style.transform = `translateX(${dx * 0.42}px) rotate(${dx * 0.008}deg)`;
    artworkWrap.style.opacity = String(Math.max(0.58, 1 - Math.abs(dx) / 430));
  });

  const endArtDrag = (event, cancelled = false) => {
    if (!artDrag) return;
    const dx = artDrag.dx;
    const wasActive = artDrag.active;
    const id = artDrag.id;
    artDrag = null;
    artworkWrap.classList.remove("is-dragging");
    artworkWrap.style.transition = "transform 280ms cubic-bezier(.22,.8,.22,1), opacity 240ms ease";
    artworkWrap.style.transform = "";
    artworkWrap.style.opacity = "";
    window.setTimeout(() => {
      artworkWrap.style.transition = "";
    }, 300);
    if (artworkWrap.hasPointerCapture(id)) {
      artworkWrap.releasePointerCapture(id);
    }
    if (!cancelled && wasActive && Math.abs(dx) > 44) {
      if (dx < 0) nextTrack();
      else previousTrack();
    }
  };

  artworkWrap.addEventListener("pointerup", (event) => endArtDrag(event, false));
  artworkWrap.addEventListener("pointercancel", (event) => endArtDrag(event, true));

  /* volume */
  document.querySelectorAll("[data-volume-range]").forEach((input) => {
    updateRange(input);
    input.addEventListener("input", () => syncVolume(Number(input.value), input));
    input.addEventListener("change", () => showToast(`Volume ${state.volume}%`));
  });

  const brightnessRange = document.getElementById("brightnessRange");
  const brightnessValue = document.getElementById("brightnessValue");
  if (brightnessRange) {
    updateRange(brightnessRange);
    brightnessRange.addEventListener("input", () => {
      state.brightness = Number(brightnessRange.value);
      brightnessValue.textContent = `${state.brightness}%`;
      updateRange(brightnessRange);
    });
    brightnessRange.addEventListener("change", () => {
      showToast(`Brightness ${state.brightness}%`);
    });
  }

  /* toggles */
  document.querySelectorAll(".toggle").forEach((toggle) => {
    toggle.addEventListener("click", () => {
      const isOn = !toggle.classList.contains("is-on");
      toggle.classList.toggle("is-on", isOn);
      toggle.setAttribute("aria-checked", String(isOn));
      showToast(`${toggle.dataset.toggleLabel || "Setting"} ${isOn ? "on" : "off"}`);
    });
  });

  /* settings actions */
  const outputRow = document.getElementById("outputRow");
  const outputValue = document.getElementById("outputValue");
  const outputs = ["Headphones", "USB DAC", "Bluetooth"];
  if (outputRow) {
    outputRow.addEventListener("click", () => {
      const currentIndex = outputs.indexOf(outputValue.textContent);
      const next = outputs[(currentIndex + 1) % outputs.length];
      outputValue.textContent = next;
      showToast(`Output: ${next}`);
    });
  }

  const eqRow = document.getElementById("eqRow");
  const eqValue = document.getElementById("eqValue");
  const eqModes = ["Studio", "Flat", "Warm", "Vocal"];
  if (eqRow) {
    eqRow.addEventListener("click", () => {
      const currentIndex = eqModes.indexOf(eqValue.textContent);
      const next = eqModes[(currentIndex + 1) % eqModes.length];
      eqValue.textContent = next;
      showToast(`Equalizer: ${next}`);
    });
  }

  const storageRow = document.getElementById("storageRow");
  if (storageRow) storageRow.addEventListener("click", () => showToast("18.2 GB free · 109.8 GB used"));
  const firmwareRow = document.getElementById("firmwareRow");
  if (firmwareRow) firmwareRow.addEventListener("click", () => showToast("POD OS is up to date"));

  /* keyboard */
  window.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      handleEscape();
      return;
    }

    const target = event.target;
    const isTyping = target instanceof HTMLElement
      && (target.tagName === "INPUT" || target.tagName === "TEXTAREA" || target.isContentEditable);
    if (isTyping) return;

    if (event.code === "Space" && !(target instanceof HTMLElement && target.closest("button"))) {
      event.preventDefault();
      togglePlay();
      return;
    }
    if (event.key === "ArrowRight") {
      event.preventDefault();
      nextTrack();
      return;
    }
    if (event.key === "ArrowLeft") {
      event.preventDefault();
      previousTrack();
      return;
    }
    if (event.key.toLowerCase() === "m") {
      if (state.screen === "now") {
        if (state.menuOpen) closeMenu();
        else openMenu();
      }
      return;
    }
    if (event.key.toLowerCase() === "l") {
      navigate("library");
      return;
    }
    if (event.key.toLowerCase() === "q") {
      navigate("queue");
      return;
    }
    if (event.key.toLowerCase() === "s") {
      navigate("settings");
      return;
    }
    if (event.key.toLowerCase() === "i") {
      navigate("listening");
      return;
    }
    if (event.key.toLowerCase() === "f") {
      openFileManager();
    }
  });

  /* simulated playback */
  window.setInterval(() => {
    if (!state.playing || state.scrubbing) return;
    state.position += 0.25;
    if (state.position >= current().duration) {
      if (state.repeat === "one") {
        state.position = 0;
        updateProgress();
      } else {
        nextTrack({ auto: true });
      }
      return;
    }
    updateProgress();
  }, 250);

  window.addEventListener("resize", fitStage);

  /* init */
  buildFolderTree();
  const initialFolder = folderMap.get(tracks[state.index].folderPath);
  state.queue = initialFolder ? initialFolder.tracks.slice() : [state.index];
  state.queueLabel = initialFolder ? initialFolder.name : "Queue";
  applyTrack(state.index, { animate: false });
  updatePlayIcons();
  setRepeat("off", false);
  setShuffle(false, false);
  renderStats();
  renderLibrary();
  renderFileManager();
  fitStage();

  const requestedScreen = query.get("screen");
  const requestedFolder = query.get("folder");
  if (requestedFolder && folderMap.has(requestedFolder) && isPathInLibrary(requestedFolder)) {
    state.libraryFolder = requestedFolder;
  }
  const requestedPath = query.get("path");
  if (requestedPath && requestedPath.startsWith("/sdcard") && folderMap.has(requestedPath)) {
    state.fileFolder = requestedPath;
  }

  if (requestedScreen && nav[requestedScreen]) {
    navigate(requestedScreen);
  } else {
    navigate("now");
  }

  const requestedTrack = Number(query.get("track"));
  if (Number.isInteger(requestedTrack) && requestedTrack >= 0 && requestedTrack < tracks.length && requestedTrack !== state.index) {
    playFromFolder(requestedTrack);
    if (requestedScreen && nav[requestedScreen]) navigate(requestedScreen);
  }

  const requestedView = query.get("view");
  if (requestedScreen === "listening") {
    setStatView(requestedView === "favorites" ? "favorites" : "top");
  }

  const requestedUI = query.get("ui");
  if (requestedUI === "menu") {
    openMenu();
  } else if (requestedUI === "pause") {
    setPlaying(false);
  }

  updateTopAction();
  document.querySelectorAll("[data-volume-range]").forEach(updateRange);
})();
