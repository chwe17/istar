$(function () {
	var iv = new iview('iview');
	var code = location.search.substr(1);
	if (!code.length) code = '3KFN';
	$.get('http://www.pdb.org/pdb/files/' + code + '.pdb', function (src) {
		iv.loadPDB(src);
	});
	$('#loadPDB').change(function () {
		$.get('http://www.pdb.org/pdb/files/' + $(this).val() + '.pdb', function (src) {
			iv.loadPDB(src);
		});
	});

	['camera', 'background', 'colorBy', 'primaryStructure', 'secondaryStructure', 'surface', 'opacity', 'wireframe', 'ligands', 'waters', 'ions', 'effect'].forEach(function (opt) {
		$('#' + opt).click(function (e) {
			var options = {};
			options[opt] = e.target.innerText;
			iv.rebuildScene(options);
			iv.render();
		})
	});

	$('#exportCanvas').click(function (e) {
		e.preventDefault();
		iv.exportCanvas();
	});
});
