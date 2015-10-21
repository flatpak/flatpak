<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version="1.0">
  <xsl:template match="@*|node()" name="identity">
    <xsl:copy>
      <xsl:apply-templates select="@*|node()" />
    </xsl:copy>
  </xsl:template>
  <xsl:template match="node/interface/method/*[1]">
    <arg type='s' name="sender" direction="in"/><xsl:text>
      </xsl:text>
    <arg type='s' name="app_id" direction="in"/><xsl:text>
      </xsl:text>
    <xsl:call-template name="identity" />
  </xsl:template>
  <xsl:template match="node/interface/signal/*[1]">
    <arg type='s' name="destination"/><xsl:text>
      </xsl:text>
    <xsl:call-template name="identity" />
  </xsl:template>
  <xsl:template match="node/interface">
    <interface>
      <xsl:attribute name="name">
        <xsl:value-of select="concat('org.freedesktop.impl.portal.', substring-after(@name,'org.freedesktop.portal.'))"/>
      </xsl:attribute>
      <xsl:apply-templates/>
    </interface>
  </xsl:template>
</xsl:stylesheet>
