<?php
	date_default_timezone_set("UTC");
	echo date('m/d/Y', strtotime('Oct 11')), "\n";
	echo date('m/d/Y', strtotime('11 Oct')), "\n";
	echo date('m/d/Y', strtotime('11 Oct 2005')), "\n";
	echo date('m/d/Y', strtotime('Oct11')), "\n";
	echo date('m/d/Y', strtotime('11Oct')), "\n";
	echo date('m/d/Y', strtotime('11Oct 2005')), "\n";
	echo date('m/d/Y', strtotime('11Oct2005')), "\n";
