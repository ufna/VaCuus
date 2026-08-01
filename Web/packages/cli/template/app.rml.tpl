<rml>
<head>
	<title>__APP_NAME__</title>
	<!-- Base sheet FIRST (the missing UA layer — div is inline without it), your
	     own sheet after so it wins ties. -->
	<link type="text/rcss" href="vacuus-base.rcss"/>
	<link type="text/rcss" href="__APP_NAME__.rcss"/>
	<!-- The bundle `vacuus build` emits. Captured at load, executed at
	     OnDocumentReady; src resolves through the ordered DevUI roots. -->
	<script src="__APP_DIR__/__APP_NAME___bundle.js"></script>
</head>
<body>
	<!-- The dedicated EMPTY mount element (E-P1). Keep it empty: pre-existing
	     children would be adopted by preact's first render, not preserved. -->
	<div id="mount"/>
</body>
</rml>
