const elements = {
	select: document.querySelector('#firmware-select'),
	status: document.querySelector('#release-status'),
	board: document.querySelector('#firmware-board'),
	version: document.querySelector('#firmware-version'),
	channel: document.querySelector('#firmware-channel'),
	description: document.querySelector('#firmware-description'),
	installButton: document.querySelector('#install-button'),
	activateButton: document.querySelector('#install-button [slot="activate"]'),
	error: document.querySelector('#installer-error')
};

let releases = [];

/**
 * Resolve a catalog path against the catalog itself. This keeps the installer
 * working on project subpaths such as GitHub Pages as well as on a custom host.
 */
function resolveCatalogPath(path) {
	return new URL(path, new URL('./firmware-catalog.json', window.location.href)).href;
}

function renderRelease(release) {
	elements.board.textContent = release.board;
	elements.version.textContent = release.version;
	elements.channel.textContent = release.channel;
	elements.description.textContent = release.description;
	elements.status.textContent = release.badge || 'Available';
	elements.installButton.manifest = resolveCatalogPath(release.manifest);
	elements.activateButton.disabled = false;
}

function showError(message) {
	elements.error.textContent = message;
	elements.error.hidden = false;
	elements.status.textContent = 'Unavailable';
	elements.select.disabled = true;
	elements.activateButton.disabled = true;
}

async function loadCatalog() {
	try {
		const response = await fetch('./firmware-catalog.json', { cache: 'no-store' });
		if (!response.ok) {
			throw new Error(`Catalog request returned ${response.status}`);
		}

		const catalog = await response.json();
		releases = catalog.firmwares.filter((release) => release.available !== false);
		if (releases.length === 0) {
			throw new Error('No installable firmware is currently published');
		}

		elements.select.replaceChildren(
			...releases.map((release) => {
				const option = document.createElement('option');
				option.value = release.id;
				option.textContent = `${release.name} · ${release.board}`;
				return option;
			})
		);
		elements.select.disabled = false;
		renderRelease(releases[0]);
	} catch (error) {
		console.error('[Notificator installer] Unable to load firmware catalog.', error);
		showError('The firmware catalog could not be loaded. Please refresh the page or try again later.');
	}
}

elements.select.addEventListener('change', (event) => {
	const release = releases.find((item) => item.id === event.target.value);
	if (release) {
		renderRelease(release);
	}
});

loadCatalog();
