<?xml version='1.0'?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:fo="http://www.w3.org/1999/XSL/Format"
                version="1.0">
  <xsl:param name="html.stylesheet" select="'docbook.css'"/>
  <xsl:param name="citerefentry.link" select="1"/>
  <xsl:template name="generate.citerefentry.link"><xsl:text>#</xsl:text><xsl:value-of select="refentrytitle"/></xsl:template>
  <xsl:param name="chapter.autolabel" select="0"></xsl:param>
</xsl:stylesheet>
