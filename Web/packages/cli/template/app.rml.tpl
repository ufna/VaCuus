<rml>
<head>
	<title>__APP_NAME__</title>
	<!-- Base sheet FIRST (the missing UA layer — div is inline without it), your
	     own sheet after so it wins ties. -->
	<link type="text/rcss" href="vacuus-base.rcss"/>
	<link type="text/rcss" href="__APP_NAME__.rcss"/>
	<!-- The bundle `vacuus build` emits. Captured at load, executed at
	     OnDocumentReady. BARE NAME, because src is DOCUMENT-RELATIVE: the head
	     handler strips this document's own filename and appends src
	     (SystemInterface::JoinPath, SystemInterface.cpp:72-83). This document
	     lives at DevUI/__APP_NAME__/, so "__APP_NAME__/__APP_NAME___bundle.js"
	     would resolve to __APP_NAME__/__APP_NAME__/__APP_NAME___bundle.js and
	     the script would be skipped with one Error — styling, no behaviour.
	     m5_hud.rml:13-18 and refhud.rml:13-16 carry the same warning. -->
	<script src="__APP_NAME___bundle.js"></script>
</head>
<body>
	<!-- The dedicated EMPTY mount element (E-P1). Keep it empty: pre-existing
	     children would be adopted by preact's first render, not preserved. -->
	<div id="mount"/>
</body>
</rml>
